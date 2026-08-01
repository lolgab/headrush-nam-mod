/* fit_image.c -- see fit_image.h.
 *
 * Writer note: a real `mkimage` leaves trailing zero slack after the
 * strings block (an internal libfdt/dtc buffer-sizing artifact, confirmed
 * empirically -- not part of the FDT spec, and no reader depends on it).
 * nam_fit_build() instead emits the tightest valid blob: header + one
 * empty mem_rsvmap terminator + struct block + strings block, sized
 * exactly. Any spec-compliant FDT reader (including nam_fit_parse() here)
 * reads both forms identically. It also replicates two `mkimage`-specific
 * ordering quirks confirmed by probing a real mkimage build byte-for-byte
 * (these aren't arbitrary, they're what a real `mkimage` actually emits
 * for this exact template):
 *   - the root node's "timestamp" property, and each hash node's "value"
 *     property, are NOT user-authored -- mkimage computes/inserts them,
 *     and does so as the FIRST property of their node (before the
 *     user-authored ones), even though their NAME strings are appended to
 *     the string table LAST (after every user-authored name).
 */
#include "fit_image.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../third_party/sha1/sha1.h"

#define FDT_MAGIC 0xD00DFEEDu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_NOP 4u
#define FDT_END 9u

static uint32_t be32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t align4(uint32_t x)
{
  return (x + 3u) & ~3u;
}

static void build_path(char* out, size_t outsz, char stack[][FIT_MAX_PROP_NAME], int depth)
{
  size_t pos = 0;
  if (outsz > 0)
    out[pos++] = '/';
  for (int i = 0; i < depth; ++i)
  {
    if (i > 0 && pos < outsz - 1)
      out[pos++] = '/';
    size_t l = strlen(stack[i]);
    if (pos + l > outsz - 1)
      l = (outsz - 1 > pos) ? (outsz - 1 - pos) : 0;
    memcpy(out + pos, stack[i], l);
    pos += l;
  }
  if (pos >= outsz)
    pos = outsz - 1;
  out[pos] = '\0';
}

static bool props_grow(FitProps* props)
{
  int new_cap = props->capacity ? props->capacity * 2 : 32;
  FitProp* n = (FitProp*)realloc(props->props, (size_t)new_cap * sizeof(FitProp));
  if (!n)
    return false;
  props->props = n;
  props->capacity = new_cap;
  return true;
}

bool nam_fit_parse(const uint8_t* data, size_t len, FitProps* out)
{
  memset(out, 0, sizeof(*out));
  if (len < 40)
    return false;
  if (be32(data) != FDT_MAGIC)
    return false;

  uint32_t off_dt_struct = be32(data + 8);
  uint32_t off_dt_strings = be32(data + 12);
  uint32_t size_dt_strings = be32(data + 32);
  uint32_t size_dt_struct = be32(data + 36);

  if ((uint64_t)off_dt_struct + size_dt_struct > len)
    return false;
  if ((uint64_t)off_dt_strings + size_dt_strings > len)
    return false;

  char stack[32][FIT_MAX_PROP_NAME];
  int depth = 0;

  uint32_t off = off_dt_struct;
  uint32_t struct_end = off_dt_struct + size_dt_struct;

  while (off < struct_end)
  {
    if ((uint64_t)off + 4 > len)
      goto fail;
    uint32_t tok = be32(data + off);
    off += 4;

    if (tok == FDT_BEGIN_NODE)
    {
      uint32_t start = off;
      uint32_t end = start;
      while (end < len && data[end] != 0)
        ++end;
      if (end >= len || depth >= 32)
        goto fail;
      size_t namelen = end - start;
      if (namelen >= FIT_MAX_PROP_NAME)
        namelen = FIT_MAX_PROP_NAME - 1;
      memcpy(stack[depth], data + start, namelen);
      stack[depth][namelen] = '\0';
      depth++;
      off = align4(end + 1);
    }
    else if (tok == FDT_END_NODE)
    {
      if (depth == 0)
        goto fail;
      depth--;
    }
    else if (tok == FDT_PROP)
    {
      if ((uint64_t)off + 8 > len)
        goto fail;
      uint32_t plen = be32(data + off);
      uint32_t nameoff = be32(data + off + 4);
      off += 8;
      uint32_t data_off = off;
      if ((uint64_t)data_off + plen > len)
        goto fail;

      uint32_t str_off = off_dt_strings + nameoff;
      if (str_off >= len)
        goto fail;
      uint32_t str_end = str_off;
      while (str_end < len && data[str_end] != 0)
        ++str_end;
      if (str_end >= len)
        goto fail;

      if (out->count >= out->capacity && !props_grow(out))
        goto fail;
      FitProp* p = &out->props[out->count++];
      build_path(p->path, sizeof(p->path), stack, depth);
      size_t nlen = str_end - str_off;
      if (nlen >= sizeof(p->name))
        nlen = sizeof(p->name) - 1;
      memcpy(p->name, data + str_off, nlen);
      p->name[nlen] = '\0';
      p->data_off = data_off;
      p->data_len = plen;

      off = align4(off + plen);
    }
    else if (tok == FDT_NOP)
    {
      /* nothing */
    }
    else if (tok == FDT_END)
    {
      break;
    }
    else
    {
      goto fail;
    }
  }
  return true;

fail:
  free(out->props);
  memset(out, 0, sizeof(*out));
  return false;
}

void nam_fit_props_free(FitProps* props)
{
  free(props->props);
  props->props = NULL;
  props->count = 0;
  props->capacity = 0;
}

bool nam_fit_get_bytes(const FitProps* props, const char* path, const char* name, uint32_t* out_off,
                        uint32_t* out_len)
{
  for (int i = 0; i < props->count; ++i)
  {
    if (strcmp(props->props[i].path, path) == 0 && strcmp(props->props[i].name, name) == 0)
    {
      if (out_off)
        *out_off = props->props[i].data_off;
      if (out_len)
        *out_len = props->props[i].data_len;
      return true;
    }
  }
  return false;
}

bool nam_fit_get_str(const uint8_t* data, const FitProps* props, const char* path, const char* name, char* out_buf,
                      size_t out_buf_size)
{
  uint32_t off, len;
  if (!nam_fit_get_bytes(props, path, name, &off, &len))
    return false;
  if (out_buf_size == 0)
    return false;
  size_t copy_len = len;
  if (copy_len >= out_buf_size)
    copy_len = out_buf_size - 1;
  memcpy(out_buf, data + off, copy_len);
  while (copy_len > 0 && out_buf[copy_len - 1] == '\0')
    --copy_len;
  out_buf[copy_len] = '\0';
  return true;
}

bool nam_fit_get_cells(const uint8_t* data, const FitProps* props, const char* path, const char* name,
                        uint32_t* out_cells, int max_cells, int* out_count)
{
  uint32_t off, len;
  if (!nam_fit_get_bytes(props, path, name, &off, &len))
    return false;
  if (len % 4 != 0)
    return false;
  int count = (int)(len / 4);
  if (count > max_cells)
    count = max_cells;
  for (int i = 0; i < count; ++i)
    out_cells[i] = be32(data + off + (uint32_t)i * 4);
  if (out_count)
    *out_count = count;
  return true;
}

bool nam_fit_read_root_metadata(const uint8_t* data, const FitProps* props, FitMetadata* out)
{
  memset(out, 0, sizeof(*out));
  if (!nam_fit_get_str(data, props, "/", "description", out->description, sizeof(out->description)))
    return false;
  if (!nam_fit_get_str(data, props, "/", "compatible", out->compatible, sizeof(out->compatible)))
    return false;
  if (!nam_fit_get_cells(data, props, "/", "inmusic,devices", out->devices, FIT_MAX_DEVICES, &out->device_count))
    return false;
  return true;
}

/* ---- writer ---- */

typedef struct
{
  uint8_t* data;
  size_t len;
  size_t cap;
} GrowBuf;

static bool gb_reserve(GrowBuf* gb, size_t extra)
{
  if (gb->len + extra <= gb->cap)
    return true;
  size_t new_cap = gb->cap ? gb->cap * 2 : 256;
  while (new_cap < gb->len + extra)
    new_cap *= 2;
  uint8_t* n = (uint8_t*)realloc(gb->data, new_cap);
  if (!n)
    return false;
  gb->data = n;
  gb->cap = new_cap;
  return true;
}

static bool gb_append(GrowBuf* gb, const void* p, size_t n)
{
  if (!gb_reserve(gb, n))
    return false;
  memcpy(gb->data + gb->len, p, n);
  gb->len += n;
  return true;
}

static bool gb_append_u32be(GrowBuf* gb, uint32_t v)
{
  uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  return gb_append(gb, b, 4);
}

static bool gb_pad_to_4(GrowBuf* gb)
{
  static const uint8_t zero[4] = {0, 0, 0, 0};
  size_t pad = (4 - (gb->len % 4)) % 4;
  return pad == 0 || gb_append(gb, zero, pad);
}

/* Fixed name list for our fixed template, in the exact order a real
 * mkimage's string table has them (source-declared names first, then the
 * two mkimage-synthesized ones last -- see this file's header comment). */
static const char* const STRTAB_NAMES[] = {"description", "compatible",  "inmusic,devices", "partition",
                                            "data",        "compression", "algo",            "timestamp",
                                            "value"};
#define STRTAB_COUNT (sizeof(STRTAB_NAMES) / sizeof(STRTAB_NAMES[0]))

static bool build_strtab(GrowBuf* strtab, uint32_t offsets[STRTAB_COUNT])
{
  for (size_t i = 0; i < STRTAB_COUNT; ++i)
  {
    offsets[i] = (uint32_t)strtab->len;
    if (!gb_append(strtab, STRTAB_NAMES[i], strlen(STRTAB_NAMES[i]) + 1))
      return false;
  }
  return true;
}

static uint32_t strtab_off(const uint32_t offsets[STRTAB_COUNT], const char* name)
{
  for (size_t i = 0; i < STRTAB_COUNT; ++i)
    if (strcmp(STRTAB_NAMES[i], name) == 0)
      return offsets[i];
  return 0; /* unreachable for this fixed template */
}

static bool emit_begin_node(GrowBuf* sb, const char* name)
{
  if (!gb_append_u32be(sb, FDT_BEGIN_NODE))
    return false;
  if (!gb_append(sb, name, strlen(name) + 1))
    return false;
  return gb_pad_to_4(sb);
}

static bool emit_end_node(GrowBuf* sb)
{
  return gb_append_u32be(sb, FDT_END_NODE);
}

static bool emit_prop(GrowBuf* sb, const uint32_t strtab_offsets[STRTAB_COUNT], const char* name, const void* data,
                       uint32_t len)
{
  if (!gb_append_u32be(sb, FDT_PROP))
    return false;
  if (!gb_append_u32be(sb, len))
    return false;
  if (!gb_append_u32be(sb, strtab_off(strtab_offsets, name)))
    return false;
  if (len > 0 && !gb_append(sb, data, len))
    return false;
  return gb_pad_to_4(sb);
}

static bool emit_image_node(GrowBuf* sb, const uint32_t strtab_offsets[STRTAB_COUNT], const char* node_name,
                            const char* description, const char* partition, const uint8_t* xz_data, size_t xz_len)
{
  uint8_t digest[20];
  sha1_buffer(xz_data, xz_len, digest);

  if (!emit_begin_node(sb, node_name))
    return false;
  if (!emit_prop(sb, strtab_offsets, "description", description, (uint32_t)strlen(description) + 1))
    return false;
  if (!emit_prop(sb, strtab_offsets, "partition", partition, (uint32_t)strlen(partition) + 1))
    return false;
  if (!emit_prop(sb, strtab_offsets, "data", xz_data, (uint32_t)xz_len))
    return false;
  if (!emit_prop(sb, strtab_offsets, "compression", "xz", 3))
    return false;
  if (!emit_begin_node(sb, "hash"))
    return false;
  if (!emit_prop(sb, strtab_offsets, "value", digest, 20))
    return false;
  if (!emit_prop(sb, strtab_offsets, "algo", "sha1", 5))
    return false;
  if (!emit_end_node(sb)) /* hash */
    return false;
  return emit_end_node(sb); /* image node */
}

bool nam_fit_build(const FitMetadata* metadata, const uint8_t* splash_xz, size_t splash_len,
                    const uint8_t* recoverysplash_xz, size_t recoverysplash_len, const uint8_t* rootfs_xz,
                    size_t rootfs_len, uint8_t** out_data, size_t* out_len)
{
  *out_data = NULL;
  *out_len = 0;

  GrowBuf strtab = {0};
  uint32_t strtab_offsets[STRTAB_COUNT];
  if (!build_strtab(&strtab, strtab_offsets))
  {
    free(strtab.data);
    return false;
  }

  uint32_t timestamp_be = (uint32_t)time(NULL);
  uint8_t timestamp_bytes[4] = {(uint8_t)(timestamp_be >> 24), (uint8_t)(timestamp_be >> 16),
                                 (uint8_t)(timestamp_be >> 8), (uint8_t)timestamp_be};

  uint8_t devices_bytes[FIT_MAX_DEVICES * 4];
  for (int i = 0; i < metadata->device_count; ++i)
  {
    uint32_t v = metadata->devices[i];
    devices_bytes[i * 4 + 0] = (uint8_t)(v >> 24);
    devices_bytes[i * 4 + 1] = (uint8_t)(v >> 16);
    devices_bytes[i * 4 + 2] = (uint8_t)(v >> 8);
    devices_bytes[i * 4 + 3] = (uint8_t)v;
  }

  GrowBuf sb = {0};
  bool ok = true;

  ok = ok && emit_begin_node(&sb, "");
  ok = ok && emit_prop(&sb, strtab_offsets, "timestamp", timestamp_bytes, 4);
  ok = ok
       && emit_prop(&sb, strtab_offsets, "description", metadata->description,
                     (uint32_t)strlen(metadata->description) + 1);
  ok = ok
       && emit_prop(&sb, strtab_offsets, "compatible", metadata->compatible,
                     (uint32_t)strlen(metadata->compatible) + 1);
  ok = ok
       && emit_prop(&sb, strtab_offsets, "inmusic,devices", devices_bytes,
                     (uint32_t)metadata->device_count * 4);
  ok = ok && emit_begin_node(&sb, "images");
  ok = ok
       && emit_image_node(&sb, strtab_offsets, "splash", "Splash screen", "splash", splash_xz, splash_len);
  ok = ok
       && emit_image_node(&sb, strtab_offsets, "recoverysplash", "Update mode splash screen", "recoverysplash",
                           recoverysplash_xz, recoverysplash_len);
  ok = ok
       && emit_image_node(&sb, strtab_offsets, "rootfs", "Root filesystem", "rootfs", rootfs_xz, rootfs_len);
  ok = ok && emit_end_node(&sb); /* images */
  ok = ok && emit_end_node(&sb); /* root */
  ok = ok && gb_append_u32be(&sb, FDT_END);

  if (!ok)
  {
    free(strtab.data);
    free(sb.data);
    return false;
  }

  uint32_t off_mem_rsvmap = 40;
  uint32_t off_dt_struct = off_mem_rsvmap + 16;
  uint32_t size_dt_struct = (uint32_t)sb.len;
  uint32_t off_dt_strings = off_dt_struct + size_dt_struct;
  uint32_t size_dt_strings = (uint32_t)strtab.len;
  uint32_t totalsize = off_dt_strings + size_dt_strings;

  uint8_t* buf = (uint8_t*)malloc(totalsize);
  if (!buf)
  {
    free(strtab.data);
    free(sb.data);
    return false;
  }

  size_t p = 0;
  uint32_t header[10] = {FDT_MAGIC,       totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap,
                          17 /* version */, 16 /* last_comp_version */, 0 /* boot_cpuid_phys */,
                          size_dt_strings, size_dt_struct};
  for (int i = 0; i < 10; ++i)
  {
    buf[p++] = (uint8_t)(header[i] >> 24);
    buf[p++] = (uint8_t)(header[i] >> 16);
    buf[p++] = (uint8_t)(header[i] >> 8);
    buf[p++] = (uint8_t)header[i];
  }
  memset(buf + p, 0, 16); /* mem_rsvmap: one zero terminator entry */
  p += 16;
  memcpy(buf + p, sb.data, sb.len);
  p += sb.len;
  memcpy(buf + p, strtab.data, strtab.len);
  p += strtab.len;

  free(strtab.data);
  free(sb.data);

  *out_data = buf;
  *out_len = totalsize;
  return true;
}
