/* modfac_spy_cli.c -- debug-only: install the ModFac_construct spy
 * (core/spy_patch.h) into an Update.img and write a new Update.img.
 * Independent of the NAM mod, so it composes with other image mods (feed it
 * the USB-console image to keep the root shell).
 *
 * usage: modfac-spy-cli <in.img> <out.img> [--registry-test NAME SHORT TYPE]
 *   --registry-test adds one extra "NAME"/"NAME 2" pedal pair built as existing type TYPE (experiment).
 */
#include "ext4_image.h"
#include "fit_image.h"
#include "spy_patch.h"
#include "tempdir.h"
#include "xz_codec.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "ERROR: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

static uint8_t* read_file(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    die("opening %s: %s", path, strerror(errno));
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc(sz > 0 ? (size_t)sz : 1);
  if (!buf || (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz))
    die("reading %s", path);
  fclose(f);
  *out_len = (size_t)sz;
  return buf;
}

static void write_file(const char* path, const uint8_t* data, size_t len)
{
  FILE* f = fopen(path, "wb");
  if (!f || (len > 0 && fwrite(data, 1, len, f) != len))
    die("writing %s", path);
  fclose(f);
}

int main(int argc, char** argv)
{
  bool meta_spy = false, modfac_spy = true, node_reg = false;
  uint32_t blob_vaddr = 0;
  const char* blob_path = NULL;
  uint64_t meta_mask = 0x1ffffffffffffull;
  NamPair pairs[NAM_MAX_PAIRS];
  uint32_t npairs = 0;
  if (argc < 3)
    die("usage: %s <in.img> <out.img> [--registry-test NAME SHORT TYPE] [--metacall-spy [IDX,IDX,..]] "
        "[--node-reg] [--no-modfac-spy]",
        argv[0]);
  for (int i = 3; i < argc; ++i)
  {
    if ((strcmp(argv[i], "--registry-test") == 0 || strcmp(argv[i], "--pair") == 0) && i + 3 < argc)
    {
      if (npairs == NAM_MAX_PAIRS)
        die("at most %d pairs", NAM_MAX_PAIRS);
      pairs[npairs].name = argv[i + 1];
      pairs[npairs].short_name = argv[i + 2];
      pairs[npairs].type_idx = (uint32_t)strtoul(argv[i + 3], NULL, 0);
      pairs[npairs].instances = 0;
      ++npairs;
      i += 3;
    }
    else if (strcmp(argv[i], "--instances") == 0 && i + 1 < argc && npairs)
      pairs[npairs - 1].instances = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--metacall-spy") == 0)
    {
      meta_spy = true;
      if (i + 1 < argc && argv[i + 1][0] != '-')
      {
        meta_mask = 0;
        for (char* t = strtok(argv[++i], ","); t; t = strtok(NULL, ","))
          meta_mask |= 1ull << (unsigned)strtoul(t, NULL, 0);
      }
    }
    else if (strcmp(argv[i], "--qml-blob") == 0 && i + 2 < argc)
    {
      blob_vaddr = (uint32_t)strtoul(argv[i + 1], NULL, 0);
      blob_path = argv[i + 2];
      i += 2;
    }
    else if (strcmp(argv[i], "--node-reg") == 0)
      node_reg = true;
    else if (strcmp(argv[i], "--no-modfac-spy") == 0)
      modfac_spy = false;
    else
      die("unknown argument: %s", argv[i]);
  }

  char err[1024] = "";
  char workdir[NAM_TEMPDIR_MAX_PATH];
  if (!nam_make_temp_dir(workdir, sizeof(workdir), err, sizeof(err)))
    die("work dir: %s", err);

  size_t in_len;
  uint8_t* in = read_file(argv[1], &in_len);

  FitProps props = {0};
  if (!nam_fit_parse(in, in_len, &props))
    die("not a valid FIT image");
  FitMetadata meta;
  if (!nam_fit_read_root_metadata(in, &props, &meta))
    die("FIT root metadata missing");

  uint32_t so, sl, ro, rl, fo, fl;
  if (!nam_fit_get_bytes(&props, "//images/splash", "data", &so, &sl)
      || !nam_fit_get_bytes(&props, "//images/recoverysplash", "data", &ro, &rl)
      || !nam_fit_get_bytes(&props, "//images/rootfs", "data", &fo, &fl))
    die("FIT image missing splash/recoverysplash/rootfs");

  uint8_t* rootfs;
  size_t rootfs_len;
  if (!nam_xz_decompress(in + fo, fl, &rootfs, &rootfs_len, err, sizeof(err)))
    die("%s", err);
  char rootfs_path[600];
  snprintf(rootfs_path, sizeof(rootfs_path), "%s/rootfs.bin", workdir);
  write_file(rootfs_path, rootfs, rootfs_len);
  free(rootfs);

  Ext4Image* img;
  if (!nam_ext4_open(rootfs_path, true, &img, err, sizeof(err)))
    die("%s", err);
  uint8_t* evil;
  size_t evil_len;
  if (!nam_ext4_dump(img, "/usr/Evil/Evil", &evil, &evil_len, err, sizeof(err)))
    die("%s", err);

  /* The registry patch parks three tables in the zero slack after the R+W segment's file range, which is the very
   * end of the file; grow the file so there is room (the extra bytes are never mapped except through that segment). */
  if (npairs)
  {
    uint8_t* grown = (uint8_t*)realloc(evil, evil_len + 4096);
    if (!grown)
      die("out of memory growing Evil");
    memset(grown + evil_len, 0, 4096);
    evil = grown;
    evil_len += 4096;
  }
  if (modfac_spy)
  {
    uint32_t tramp;
    if (!nam_elf_patch_modfac_spy(evil, evil_len, &tramp, err, sizeof(err)))
      die("%s", err);
    printf("OK  ModFac vtable slot 7 -> trampoline @ 0x%x\n", tramp);
  }
  /* registry first: it needs the largest contiguous run of the code cave */
  if (npairs)
  {
    if (!nam_elf_patch_registry(evil, evil_len, pairs, npairs, err, sizeof(err)))
      die("%s", err);
    for (uint32_t p = 0; p < npairs; ++p)
      printf("OK  registry: added \"%s\" / \"%s 2\" as type %u\n", pairs[p].name, pairs[p].name, (unsigned)pairs[p].type_idx);
  }
  if (node_reg && npairs)
  {
    if (!nam_elf_patch_node_registration(evil, evil_len, pairs, npairs, NULL, NULL, err, sizeof(err)))
      die("%s", err);
    printf("OK  property nodes registered for %u pair(s)\n", (unsigned)npairs);
  }
  if (blob_path)
  {
    /* debug: overwrite a qCompress'd QML resource ([u32 BE rcc length][u32 BE size][zlib]) in place, same length */
    size_t blen;
    uint8_t* blob = read_file(blob_path, &blen);
    uint32_t off = blob_vaddr - 0x8000u - 4u;
    uint32_t rcc = ((uint32_t)evil[off - 4] << 24) | ((uint32_t)evil[off - 3] << 16) | ((uint32_t)evil[off - 2] << 8)
                   | evil[off - 1];
    if ((size_t)off + blen > evil_len || rcc != blen)
      die("qml blob: length %zu does not match the resource's %u bytes", blen, (unsigned)rcc);
    memcpy(evil + off, blob, blen);
    free(blob);
    printf("OK  QML resource @ 0x%x replaced (%zu bytes)\n", (unsigned)blob_vaddr, blen);
  }
  if (meta_spy)
  {
    if (!nam_elf_patch_metacall_spy(evil, evil_len, meta_mask, err, sizeof(err)))
      die("%s", err);
    printf("OK  metacall spy installed (mask 0x%llx)\n", (unsigned long long)meta_mask);
  }

  if (!nam_ext4_inject(img, "/usr/Evil/Evil", evil, evil_len, 0755, err, sizeof(err)))
    die("%s", err);
  if (!nam_ext4_verify_basic(img, err, sizeof(err)))
    die("%s", err);
  nam_ext4_close(img);

  size_t patched_len;
  uint8_t* patched = read_file(rootfs_path, &patched_len);
  uint8_t* patched_xz;
  size_t patched_xz_len;
  if (!nam_xz_compress(patched, patched_len, &patched_xz, &patched_xz_len, err, sizeof(err)))
    die("%s", err);
  free(patched);

  uint8_t* out;
  size_t out_len;
  if (!nam_fit_build(&meta, in + so, sl, in + ro, rl, patched_xz, patched_xz_len, &out, &out_len))
    die("building FIT");

  /* round trip: the rebuilt image's Evil must equal what we patched */
  FitProps vp = {0};
  uint32_t vo, vl;
  if (!nam_fit_parse(out, out_len, &vp) || !nam_fit_get_bytes(&vp, "//images/rootfs", "data", &vo, &vl))
    die("round-trip: can't re-parse output");
  uint8_t* vroot;
  size_t vroot_len;
  if (!nam_xz_decompress(out + vo, vl, &vroot, &vroot_len, err, sizeof(err)))
    die("%s", err);
  char vpath[600];
  snprintf(vpath, sizeof(vpath), "%s/verify.bin", workdir);
  write_file(vpath, vroot, vroot_len);
  free(vroot);
  Ext4Image* vimg;
  if (!nam_ext4_open(vpath, false, &vimg, err, sizeof(err)))
    die("%s", err);
  uint8_t* vevil;
  size_t vevil_len;
  if (!nam_ext4_dump(vimg, "/usr/Evil/Evil", &vevil, &vevil_len, err, sizeof(err)))
    die("%s", err);
  if (vevil_len != evil_len || memcmp(vevil, evil, evil_len) != 0)
    die("round-trip FAILED: repacked Evil differs");
  nam_ext4_close(vimg);
  printf("OK  round-trip verified\n");

  write_file(argv[2], out, out_len);
  printf("OK  wrote %s (%zu bytes)\n", argv[2], out_len);
  nam_remove_dir_recursive(workdir);
  return 0;
}
