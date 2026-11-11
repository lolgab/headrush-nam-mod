/* spy_patch.c -- see spy_patch.h. */
#include "spy_patch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PT_LOAD 1u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define VADDR_BASE 0x8000u
#define PHDR_OFF_FILESZ 16u
#define PHDR_OFF_MEMSZ 20u

#define MODFAC_VTABLE 0x18399c8u
#define MODFAC_CONSTRUCT_SLOT 7u
#define MODFAC_CONSTRUCT_FN 0x209bc8u
#define ORIG_LIT_OFF 0x80u /* offset of the .word holding MODFAC_CONSTRUCT_FN inside the trampoline */

/* Assembled from the spy trampoline source (ARM, 148 bytes incl. literals):
 * saves r0-r7/r12/lr, builds the record on the stack, open(O_APPEND)/write/
 * close on /tmp/modfac.bin, restores everything, jumps to the original. */
static const uint8_t k_spy_tramp[148] = {
  0xff, 0x50, 0x2d, 0xe9, 0x40, 0xd0, 0x4d, 0xe2, 0x68, 0x40, 0x8d, 0xe2,
  0x40, 0x50, 0x9d, 0xe5, 0x44, 0x60, 0x9d, 0xe5, 0x64, 0x70, 0x9d, 0xe5,
  0xe0, 0x00, 0x8d, 0xe8, 0x0c, 0x40, 0x8d, 0xe5, 0x10, 0x50, 0x8d, 0xe2,
  0x0f, 0x00, 0xb4, 0xe8, 0x0f, 0x00, 0xa5, 0xe8, 0x0f, 0x00, 0xb4, 0xe8,
  0x0f, 0x00, 0xa5, 0xe8, 0x0f, 0x00, 0xb4, 0xe8, 0x0f, 0x00, 0xa5, 0xe8,
  0x40, 0x00, 0x8f, 0xe2, 0x41, 0x14, 0x00, 0xe3, 0x69, 0x2f, 0xa0, 0xe3,
  0x05, 0x70, 0xa0, 0xe3, 0x00, 0x00, 0x00, 0xef, 0x00, 0x40, 0xb0, 0xe1,
  0x06, 0x00, 0x00, 0x4a, 0x0d, 0x10, 0xa0, 0xe1, 0x40, 0x20, 0xa0, 0xe3,
  0x04, 0x70, 0xa0, 0xe3, 0x00, 0x00, 0x00, 0xef, 0x04, 0x00, 0xa0, 0xe1,
  0x06, 0x70, 0xa0, 0xe3, 0x00, 0x00, 0x00, 0xef, 0x40, 0xd0, 0x8d, 0xe2,
  0xff, 0x50, 0xbd, 0xe8, 0x04, 0xf0, 0x1f, 0xe5, 0xc8, 0x9b, 0x20, 0x00,
  0x2f, 0x74, 0x6d, 0x70, 0x2f, 0x6d, 0x6f, 0x64, 0x66, 0x61, 0x63, 0x2e,
  0x62, 0x69, 0x6e, 0x00,
};

static uint32_t rd32(const uint8_t* d, size_t off)
{
  return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) | ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
}

static void wr32(uint8_t* d, size_t off, uint32_t v)
{
  d[off] = (uint8_t)v;
  d[off + 1] = (uint8_t)(v >> 8);
  d[off + 2] = (uint8_t)(v >> 16);
  d[off + 3] = (uint8_t)(v >> 24);
}

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

/* Reserves `need` bytes (word-aligned) of dead space right after the R+E
 * LOAD segment's file end, the same reclaimed gap patch_gonkulator uses, and
 * grows that segment's filesz/memsz to cover it. The bytes must be zero and
 * not inside any other PT_LOAD's file range. Returns file offset and vaddr. */
static bool cave_alloc(uint8_t* data, size_t data_len, uint32_t need, uint32_t* out_off, uint32_t* out_vaddr,
                       char* err, size_t err_size)
{
  uint32_t e_phoff = rd32(data, 28);
  uint16_t e_phentsize = (uint16_t)(data[42] | (data[43] << 8));
  uint16_t e_phnum = (uint16_t)(data[44] | (data[45] << 8));
  if ((size_t)e_phoff + (size_t)e_phentsize * e_phnum > data_len)
  {
    set_err(err, err_size, "program header table out of bounds");
    return false;
  }

  need = (need + 3u) & ~3u;
  int code_idx = -1, code_count = 0;
  for (uint16_t i = 0; i < e_phnum; ++i)
  {
    size_t off = (size_t)e_phoff + (size_t)i * e_phentsize;
    if (rd32(data, off) == PT_LOAD && rd32(data, off + 24) == (PF_R | PF_X))
    {
      code_idx = i;
      code_count++;
    }
  }
  if (code_count != 1)
  {
    set_err(err, err_size, "REFUSING: expected exactly one plain R+E LOAD segment, found %d", code_count);
    return false;
  }
  size_t cph = (size_t)e_phoff + (size_t)code_idx * e_phentsize;
  uint32_t c_off = rd32(data, cph + 4), c_vaddr = rd32(data, cph + 8);
  uint32_t c_filesz = rd32(data, cph + 16), c_memsz = rd32(data, cph + 20);
  if (c_filesz != c_memsz)
  {
    set_err(err, err_size, "REFUSING: code segment filesz != memsz");
    return false;
  }

  uint32_t cave_off = c_off + c_filesz;
  if ((size_t)cave_off + need > data_len)
  {
    set_err(err, err_size, "REFUSING: code cave runs past end of file");
    return false;
  }
  for (uint16_t i = 0; i < e_phnum; ++i)
  {
    size_t off = (size_t)e_phoff + (size_t)i * e_phentsize;
    if (rd32(data, off) != PT_LOAD || (int)i == code_idx)
      continue;
    uint32_t o = rd32(data, off + 4);
    if (o < cave_off + need && o + rd32(data, off + 16) > cave_off)
    {
      set_err(err, err_size, "REFUSING: code cave overlaps another PT_LOAD segment");
      return false;
    }
  }
  for (uint32_t i = 0; i < need; ++i)
  {
    if (data[cave_off + i] != 0)
    {
      set_err(err, err_size, "REFUSING: code cave not zero at file offset 0x%x (already patched?)",
              (unsigned)(cave_off + i));
      return false;
    }
  }

  wr32(data, cph + PHDR_OFF_FILESZ, c_filesz + need);
  wr32(data, cph + PHDR_OFF_MEMSZ, c_memsz + need);
  *out_off = cave_off;
  *out_vaddr = c_vaddr + c_filesz;
  return true;
}

bool nam_elf_patch_modfac_spy(uint8_t* data, size_t data_len, uint32_t* out_tramp, char* err, size_t err_size)
{
  if (data_len < 52 || memcmp(data, "\x7f" "ELF", 4) != 0 || data[4] != 1 || data[5] != 1)
  {
    set_err(err, err_size, "not a 32-bit little-endian ELF");
    return false;
  }
  uint32_t slot_vaddr = MODFAC_VTABLE + MODFAC_CONSTRUCT_SLOT * 4;
  uint32_t slot_off = slot_vaddr - VADDR_BASE;
  if ((size_t)slot_off + 4 > data_len || rd32(data, slot_off) != MODFAC_CONSTRUCT_FN)
  {
    set_err(err, err_size,
            "REFUSING: ModFac vtable slot @ 0x%x does not hold 0x%x (wrong binary / already patched / not "
            "Pedalboard 2.7).",
            (unsigned)slot_vaddr, (unsigned)MODFAC_CONSTRUCT_FN);
    return false;
  }

  uint32_t cave_off, tramp_vaddr;
  if (!cave_alloc(data, data_len, (uint32_t)sizeof(k_spy_tramp), &cave_off, &tramp_vaddr, err, err_size))
    return false;
  uint32_t len = (uint32_t)sizeof(k_spy_tramp);
  memcpy(data + cave_off, k_spy_tramp, len);
  if (rd32(data, cave_off + ORIG_LIT_OFF) != MODFAC_CONSTRUCT_FN)
  {
    set_err(err, err_size, "internal error: trampoline literal offset is wrong");
    return false;
  }
  wr32(data, slot_off, tramp_vaddr);

  if (out_tramp)
    *out_tramp = tramp_vaddr;
  return true;
}

/* ---- pedal registry: add one extra "<name>" / "<name> 2" pair -----------------------------------------------
 *
 * The UI identifies a pedal by its index k into the name array: 0 = "Empty Slot", 1..166 = normal pedals (each
 * type listed twice, "X" and "X 2"), 167 = "FX-Loop" (special). Everything per-pedal is a parallel 168-word table
 * indexed by k (names, short names, category X, descriptor tables P1-P4, T5), the ctor's type-index array, and
 * a pile of compares against 0xa5..0xa8 that mean "normal range" / "is FX-Loop". Appending a pair therefore means:
 * every table grows by two entries (FX-Loop moves from 167 to 169), every reader is repointed at the new copy,
 * and the hard-coded constants are bumped by 2. Tables that cannot be extended in place are relocated into the
 * R+E gap and the zero slack at the end of the R+W segment. Every patched word is checked against its expected
 * old value first. */

#define REG_VADDR_TYPES_START_LIT 0x6254c4u /* ctor literal: types array start (points at the header word) */
#define REG_VADDR_TYPES_END_LIT 0x6254c8u   /* ctor literal: types array end (points at the last entry)   */
#define REG_NAMES_LIT_CTOR 0x6254ccu        /* ctor literal: name array base - 4                          */
#define REG_NAMES_LIT_MAP 0xbdbb0u          /* fcn 0xbcf68 literal: name array base - 4                   */
#define REG_MAP_END_INSN 0xbcf70u           /* add r7, r6, #0x2a0 -> end of the name array in that fcn    */
#define REG_MAP_END_INSN_OLD 0xe2867e2au
#define REG_CMP_LAST_IDX 0x62588cu       /* ctor: cmp r4, #0xa7 */
#define REG_CMP_LAST_IDX_OLD 0xe35400a7u
#define REG_CMP_COUNT 0x62589cu /* ctor: cmp r6, #0xa8 */
#define REG_CMP_COUNT_OLD 0xe35600a8u
#define REG_MEMSET_SZ 0x625084u /* ctor: mov r2, #0x298 (in-object prototype table) */
#define REG_MEMSET_SZ_OLD 0xe3a02fa6u
#define REG_SHORT_LOOP_CMP 0x3d8760u  /* cmp sl, #0xa7 */
#define REG_SHORT_LOOP_CMP_OLD 0xe35a00a7u

#define REG_TYPES_START 0x2fbb5fcu
#define REG_TYPES_END 0x2fbb894u
#define REG_NAMES_BASE 0x1609f28u
#define REG_SHORT_BASE 0x185fcb8u
#define REG_NAME_COUNT 168u
#define REG_TYPE_COUNT 166u
#define REG_TABLE_BYTES (REG_NAME_COUNT * 4u)

/* movw/movt readers of the name array base 0x1609f28 */
static const uint32_t k_names_movw_sites[] = {0x87b58u, 0xbce28u, 0x3d873cu, 0x62563cu};
/* literal readers of names+16 (loops over names from index 5) */
static const uint32_t k_names16_lits[] = {0x3db6a0u, 0x3e6a04u, 0x3f2be8u};
#define REG_SHORT_MOVW 0x3d87c0u


/* ---- picker catalog hook ----------------------------------------------------------------------------------------
 *
 * The block-type picker's category -> pedal-name lists are built by a static initializer (fcn 0x66288) from
 * literal names: arrays of std::string in consecutive stack slots, turned into vectors by fcn 0x59c0f0(out, array,
 * count). The "Distortion" list is built by the `bl 0x59c0f0` at 0x665ac (count 0x12). The arrays cannot grow in
 * place, so that call is redirected through a veneer (parked in the dead body of ModFac_construct case 84, a type
 * no registry entry builds) to a hook in the cave. The hook rebuilds the array on its own stack with the extra
 * names appended and calls the original builder with the larger count. */
#define CAT_CALL_SITE 0x665acu
#define CAT_CALL_SITE_OLD 0xeb14d6cfu /* bl 0x59c0f0 */
#define CAT_VENEER 0x209e28u          /* start of the dead case-84 body */
#define CAT_VENEER_CASE_SLOT (0x209bf0u + 84u * 4u)
#define CAT_STR_RANGE_CTOR 0x5b48e0u
#define CAT_STR_CSTR_CTOR 0x5b2e84u
#define CAT_VEC_BUILD 0x59c0f0u
#define CAT_HOOK_LIT_OFF 196u

static const uint8_t k_cat_hook[208] = {
  0xf0, 0x5f, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1, 0x01, 0x50, 0xa0, 0xe1,
  0x02, 0x60, 0xa0, 0xe1, 0x03, 0x70, 0xa0, 0xe1, 0x00, 0x80, 0x97, 0xe5,
  0x08, 0x90, 0x86, 0xe0, 0x18, 0x00, 0xa0, 0xe3, 0x99, 0x00, 0x0b, 0xe0,
  0x0b, 0xd0, 0x4d, 0xe0, 0x0d, 0xa0, 0xa0, 0xe1, 0x00, 0xb0, 0xa0, 0xe3,
  0x06, 0x00, 0x5b, 0xe1, 0x0c, 0x00, 0x00, 0xaa, 0x18, 0x30, 0xa0, 0xe3,
  0x9b, 0x03, 0x03, 0xe0, 0x03, 0x00, 0x8a, 0xe0, 0x03, 0x10, 0x85, 0xe0,
  0x08, 0x30, 0x80, 0xe2, 0x00, 0x30, 0x80, 0xe5, 0x04, 0x20, 0x91, 0xe5,
  0x00, 0x10, 0x91, 0xe5, 0x02, 0x20, 0x81, 0xe0, 0x60, 0xc0, 0x9f, 0xe5,
  0x3c, 0xff, 0x2f, 0xe1, 0x01, 0xb0, 0x8b, 0xe2, 0xf0, 0xff, 0xff, 0xea,
  0x00, 0xb0, 0xa0, 0xe3, 0x08, 0x00, 0x5b, 0xe1, 0x09, 0x00, 0x00, 0xaa,
  0x0b, 0x30, 0x86, 0xe0, 0x18, 0x20, 0xa0, 0xe3, 0x93, 0x02, 0x03, 0xe0,
  0x03, 0x00, 0x8a, 0xe0, 0x04, 0x10, 0x87, 0xe2, 0x0b, 0x11, 0x91, 0xe7,
  0x30, 0xc0, 0x9f, 0xe5, 0x3c, 0xff, 0x2f, 0xe1, 0x01, 0xb0, 0x8b, 0xe2,
  0xf3, 0xff, 0xff, 0xea, 0x04, 0x00, 0xa0, 0xe1, 0x0a, 0x10, 0xa0, 0xe1,
  0x09, 0x20, 0xa0, 0xe1, 0x18, 0xc0, 0x9f, 0xe5, 0x3c, 0xff, 0x2f, 0xe1,
  0x18, 0x30, 0xa0, 0xe3, 0x99, 0x03, 0x03, 0xe0, 0x03, 0xd0, 0x8d, 0xe0,
  0xf0, 0x9f, 0xbd, 0xe8, 0xe0, 0x48, 0x5b, 0x00, 0x84, 0x2e, 0x5b, 0x00,
  0xf0, 0xc0, 0x59, 0x00,
};

typedef enum
{
  PLACE_IN_PLACE, /* extend where it is: the 8 bytes past its end are the (relocated) neighbor's first entries */
  PLACE_CAVE,     /* relocate into the R+E gap */
  PLACE_RW_TAIL   /* relocate into the zero slack at the end of the R+W segment's file-backed range */
} Place;

typedef struct
{
  uint32_t base;
  const char* what;
  Place place;
  uint32_t sites[2]; /* movw/movt readers to repoint (0-terminated), unused when in place */
  bool distinct_pair; /* entries 165/166 are different pointers ("X"/"X 2" editor paths), not equal */
} IdTable;

static const IdTable k_id_tables[] = {
    {0x1609c88u, "category table X", PLACE_IN_PLACE, {0, 0}, false},  /* neighbor: old name array [0],[1] */
    {0x185fa18u, "descriptor table P1", PLACE_IN_PLACE, {0, 0}, false}, /* neighbor: old short-name array [0],[1] */
    {0x1860108u, "descriptor table P2", PLACE_CAVE, {0x3e68e0u, 0}, false},
    {0x1860700u, "descriptor table P3", PLACE_RW_TAIL, {0x3f2ac0u, 0}, false},
    {0x18631d0u, "descriptor table P4", PLACE_RW_TAIL, {0x419d20u, 0x41a7b0u}, false},
    {0x18623a8u, "table T5", PLACE_RW_TAIL, {0x4125d4u, 0x41b6b8u}, false},
    /* editor page paths "../Pages/ModuleEditors/<File>"; ends where the old P4 begins (relocated above) */
    {0x1862f30u, "editor path table T7", PLACE_IN_PLACE, {0, 0}, true},
};

/* hard-coded id-space constants: {vaddr, old word, new word} */
typedef enum
{
  ID_SHIFT, /* +1 per extra id: the special ids (FX-Loop, Input, Output, Mix) and normal-range bounds */
  ID_PAIR,  /* +1 per extra picker row: picker row thresholds */
  ID_TAIL,  /* +(extra ids - extra rows): picker row -> id offset for the rows after the extra ones */
  ID_CAP    /* +4 bytes per extra id: capacity of id-indexed vectors / the prototype table */
} IdKind;

static const struct
{
  uint32_t vaddr, old_w;
  IdKind kind;
} k_id_sites[] = {
    {0xb7344, 0xe35400a7, ID_SHIFT},
    {0xb7354, 0xe35200a5, ID_SHIFT},
    {0xb7368, 0xe35000a5, ID_SHIFT},
    {0xb7370, 0xe35100a7, ID_SHIFT},
    {0xb7374, 0x035400a7, ID_SHIFT},
    {0xb7390, 0xe35000a5, ID_SHIFT},
    {0xb7398, 0xe35400a7, ID_SHIFT},
    {0xb739c, 0x035100a7, ID_SHIFT},
    {0xb73ac, 0xe35000a5, ID_SHIFT},
    {0xb73b4, 0xe35100a7, ID_SHIFT},
    {0xb73b8, 0x035400a7, ID_SHIFT},
    {0xb73c8, 0xe35000a5, ID_SHIFT},
    {0xb73d0, 0xe35100a7, ID_SHIFT},
    {0xb73d4, 0x035400a7, ID_SHIFT},
    {0xb73e4, 0xe35000a5, ID_SHIFT},
    {0xb73ec, 0xe35400a7, ID_SHIFT},
    {0xb73f0, 0x035100a7, ID_SHIFT},
    {0xb7400, 0xe35000a5, ID_SHIFT},
    {0xb7408, 0xe35400a7, ID_SHIFT},
    {0xb740c, 0x035100a7, ID_SHIFT},
    {0xb741c, 0xe35000a5, ID_SHIFT},
    {0xb7424, 0xe35400a7, ID_SHIFT},
    {0xb7428, 0x035100a7, ID_SHIFT},
    {0xb7438, 0xe35000a5, ID_SHIFT},
    {0xb7440, 0xe35400a7, ID_SHIFT},
    {0xb7444, 0x035100a7, ID_SHIFT},
    {0xb7454, 0xe35000a5, ID_SHIFT},
    {0xb745c, 0xe35400a7, ID_SHIFT},
    {0xb7460, 0x035100a7, ID_SHIFT},
    {0xb7470, 0xe35100a5, ID_SHIFT},
    {0xb7478, 0xe35400a7, ID_SHIFT},
    {0xb747c, 0x035300a7, ID_SHIFT},
    {0xb7524, 0xe35000a5, ID_SHIFT},
    {0xb7558, 0xe35000a5, ID_SHIFT},
    {0xb7584, 0xe35000a5, ID_SHIFT},
    {0xb75b0, 0xe35000a5, ID_SHIFT},
    {0xb75dc, 0xe35000a5, ID_SHIFT},
    {0xb7608, 0xe35000a5, ID_SHIFT},
    {0xb7634, 0xe35000a5, ID_SHIFT},
    {0xb7660, 0xe35000a5, ID_SHIFT},
    {0xb768c, 0xe35000a5, ID_SHIFT},
    {0xb76b4, 0xe35100a5, ID_SHIFT},
    {0xb77cc, 0xe35200a5, ID_SHIFT},
    {0xb77d4, 0xe35300a7, ID_SHIFT},
    {0xb780c, 0xe35200a5, ID_SHIFT},
    {0x3b7228, 0xe35600a7, ID_SHIFT},
    {0x3b73d0, 0xe35600a7, ID_SHIFT},
    {0x3c8314, 0x135600a7, ID_SHIFT},
    {0xbcd3c, 0xe35300a6, ID_SHIFT},
    {0x3cd840, 0xe35200a6, ID_SHIFT},
    {0x3cd84c, 0xe35200a6, ID_SHIFT},
    {0x3ce144, 0xe35300a5, ID_SHIFT},
    {0x417cec, 0xe35100a7, ID_SHIFT},
    {0x417d30, 0xe35300a7, ID_SHIFT},
    {0x417d74, 0xe35300a7, ID_SHIFT},
    {0x417db8, 0xe35300a7, ID_SHIFT},
    {0x417dfc, 0xe35300a7, ID_SHIFT},
    {0x417e40, 0xe35300a7, ID_SHIFT},
    {0x417e90, 0xe35300a7, ID_SHIFT},
    {0x417ee0, 0xe35300a7, ID_SHIFT},
    {0x417f30, 0xe35300a7, ID_SHIFT},
    {0x417f74, 0xe35300a7, ID_SHIFT},
    {0x417fb8, 0xe35300a7, ID_SHIFT},
    {0x3db530, 0xe35500a8, ID_SHIFT},
    {0x3e68c4, 0xe35400a8, ID_SHIFT},
    {0x3f2aa4, 0xe35500a8, ID_SHIFT},
    {0xb77e4, 0xe3a030a7, ID_SHIFT},
    {0xbcd70, 0xe3a020a7, ID_SHIFT},
    {0x3cd4ec, 0xe3a010a7, ID_SHIFT},
    {0xbcd88, 0xe3a02053, ID_PAIR},
    {0x3cdd10, 0xe3530052, ID_PAIR},
    {0x3cddd8, 0xe3560053, ID_PAIR},
    {0x3cdddc, 0xc2866053, ID_TAIL},
    {0x5f9f74, 0xe3a030a7, ID_SHIFT},
    {0x5f9ed4, 0xe3a030a8, ID_SHIFT},
    {0x5f9f24, 0xe3a030a9, ID_SHIFT},
    {0x5f9e84, 0xe3a030aa, ID_SHIFT},
    {0x5f9e28, 0xe3530fa9, ID_CAP},
    {0x60490c, 0xe3a00faa, ID_CAP},
    {0x6049d4, 0xe2855faa, ID_CAP},
    {0x5f9e3c, 0xe3550faa, ID_CAP},
    {0x5f9e44, 0xe3550fab, ID_CAP},
    {0x6047c0, 0xe2835fab, ID_CAP},
    {0x6047fc, 0xe26260ab, ID_SHIFT},
    {0x604be0, 0xe2866fab, ID_CAP},
    {0x5f9e58, 0xe3530fa9, ID_CAP},
    {0x604830, 0xe3a00faa, ID_CAP},
    {0x6048f8, 0xe2855faa, ID_CAP},
    {0x5f9e6c, 0xe3a020ab, ID_SHIFT},
    {0x40020c, 0xe35400ab, ID_SHIFT},
    {0x412508, 0xe35300a2, ID_SHIFT},
    {0x41a78c, 0xe35200a2, ID_SHIFT},
    {0x41b590, 0xe35300a2, ID_SHIFT}
};

/* Reserves `need` bytes (word-aligned) of zero slack after the R+W LOAD segment's file-backed end, before the next
 * page boundary, and grows that segment's p_filesz to cover them. The file bytes there are trailing non-loaded
 * data (section tables) and get overwritten. */
static bool rw_tail_alloc(uint8_t* data, size_t data_len, uint32_t need, uint32_t* out_off, uint32_t* out_vaddr,
                          char* err, size_t err_size)
{
  uint32_t e_phoff = rd32(data, 28);
  uint16_t e_phentsize = (uint16_t)(data[42] | (data[43] << 8));
  uint16_t e_phnum = (uint16_t)(data[44] | (data[45] << 8));
  need = (need + 3u) & ~3u;
  int rw = -1, count = 0;
  for (uint16_t i = 0; i < e_phnum; ++i)
  {
    size_t off = (size_t)e_phoff + (size_t)i * e_phentsize;
    if (rd32(data, off) == PT_LOAD && rd32(data, off + 24) == (PF_R | PF_W))
    {
      rw = i;
      count++;
    }
  }
  if (count != 1)
  {
    set_err(err, err_size, "REFUSING: expected exactly one plain R+W LOAD segment, found %d", count);
    return false;
  }
  size_t ph = (size_t)e_phoff + (size_t)rw * e_phentsize;
  uint32_t p_off = rd32(data, ph + 4), p_vaddr = rd32(data, ph + 8);
  uint32_t filesz = rd32(data, ph + 16), memsz = rd32(data, ph + 20);
  uint32_t vend = p_vaddr + filesz;
  uint32_t page_end = (vend + 0xfffu) & ~0xfffu;
  if (filesz + need > memsz || vend + need > page_end || (size_t)p_off + filesz + need > data_len)
  {
    set_err(err, err_size,
            "REFUSING: not enough slack after the R+W segment (need %u, have %u before the page end)", (unsigned)need,
            (unsigned)(page_end - vend));
    return false;
  }
  wr32(data, ph + 16, filesz + need);
  *out_off = p_off + filesz;
  *out_vaddr = vend;
  memset(data + *out_off, 0, need);
  return true;
}

/* Rewrites a movw/movt pair at `vaddr` (movw) / vaddr+4 (movt) from `old_val` to `new_val`, same register. */
static bool patch_movw_movt(uint8_t* data, size_t data_len, uint32_t vaddr, uint32_t old_val, uint32_t new_val,
                            char* err, size_t err_size)
{
  uint32_t off = vaddr - VADDR_BASE;
  if ((size_t)off + 8 > data_len)
  {
    set_err(err, err_size, "movw/movt site 0x%x out of bounds", (unsigned)vaddr);
    return false;
  }
  uint32_t lo = rd32(data, off), hi = rd32(data, off + 4);
  uint32_t lo_val = ((lo >> 4) & 0xF000u) | (lo & 0xFFFu);
  uint32_t hi_val = ((hi >> 4) & 0xF000u) | (hi & 0xFFFu);
  uint32_t rd_lo = (lo >> 12) & 15u, rd_hi = (hi >> 12) & 15u;
  if ((lo & 0x0FF00000u) != 0x03000000u || (hi & 0x0FF00000u) != 0x03400000u || rd_lo != rd_hi
      || ((hi_val << 16) | lo_val) != old_val)
  {
    set_err(err, err_size, "REFUSING: movw/movt @ 0x%x does not load 0x%x (wrong binary / already patched).",
            (unsigned)vaddr, (unsigned)old_val);
    return false;
  }
  wr32(data, off, (lo & 0xFFF0F000u) | (((new_val >> 12) & 0xFu) << 16) | (new_val & 0xFFFu));
  wr32(data, off + 4, (hi & 0xFFF0F000u) | ((((new_val >> 16) >> 12) & 0xFu) << 16) | ((new_val >> 16) & 0xFFFu));
  return true;
}

static bool check_word(const uint8_t* data, size_t data_len, uint32_t vaddr, uint32_t want, const char* what, char* err,
                       size_t err_size)
{
  uint32_t off = vaddr - VADDR_BASE;
  if ((size_t)off + 4 > data_len || rd32(data, off) != want)
  {
    set_err(err, err_size, "REFUSING: %s @ 0x%x does not hold 0x%x (wrong binary / already patched).", what,
            (unsigned)vaddr, (unsigned)want);
    return false;
  }
  return true;
}

#define NAM_MAX_IDS 8u

/* Instances (ids) one pedal entry owns: 2 (a pair) or 4. */
static uint32_t nam_pair_instances(const NamPair* p)
{
  return p->instances ? p->instances : 2;
}

/* Validates the pedal list and returns the number of extra ids. One entry may own 4 ids, but then it must be the only
 * entry: the picker maps a row to its first id as 2*row-1, which only holds for pairs and for the first extra row. */
static bool nam_pairs_ids(const NamPair* pairs, uint32_t n, uint32_t* out_ids, char* err, size_t err_size)
{
  uint32_t ids = 0;
  if (n < 1 || n > NAM_MAX_PAIRS)
  {
    set_err(err, err_size, "pedal count %u out of range 1..%u", (unsigned)n, (unsigned)NAM_MAX_PAIRS);
    return false;
  }
  for (uint32_t p = 0; p < n; ++p)
  {
    uint32_t inst = nam_pair_instances(&pairs[p]);
    if (inst != 2 && inst != 4)
    {
      set_err(err, err_size, "a pedal has 2 or 4 instances, not %u", (unsigned)inst);
      return false;
    }
    if (inst == 4 && n != 1)
    {
      set_err(err, err_size, "a 4-instance pedal must be the only extra pedal");
      return false;
    }
    ids += inst;
  }
  if (ids > NAM_MAX_IDS)
  {
    set_err(err, err_size, "at most %u extra ids", (unsigned)NAM_MAX_IDS);
    return false;
  }
  *out_ids = ids;
  return true;
}

static uint32_t id_site_add(int kind, uint32_t ids, uint32_t rows)
{
  return kind == ID_PAIR ? rows : kind == ID_TAIL ? ids - rows : kind == ID_SHIFT ? ids : 4 * ids;
}

/* Copies old[0..166], then old[165], old[166] alternating for every extra id (an extra pedal clones the last normal
 * pair: odd ids are "X", even ids "X 2"), then old[167] (FX-Loop). In-place extension is safe because the three
 * source words are read first. */
static void extend_table(uint8_t* data, uint32_t old_off, uint32_t dst_off, uint32_t nids)
{
  uint32_t t165 = rd32(data, old_off + 165 * 4), t166 = rd32(data, old_off + 166 * 4), t167 = rd32(data, old_off + 167 * 4);
  if (dst_off != old_off)
    for (uint32_t i = 0; i < 167; ++i)
      wr32(data, dst_off + i * 4, rd32(data, old_off + i * 4));
  for (uint32_t i = 0; i < nids; ++i)
    wr32(data, dst_off + (167 + i) * 4, (i & 1) ? t166 : t165);
  wr32(data, dst_off + (167 + nids) * 4, t167);
}

/* Returns an ARM data-processing word whose rotated 8-bit immediate is the old one plus `add` (0 if not encodable). */
static uint32_t bump_imm(uint32_t w, uint32_t add)
{
  uint32_t imm = w & 0xffu, rot = ((w >> 8) & 15u) * 2u;
  uint32_t val = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
  val += add;
  for (uint32_t r = 0; r < 16; ++r)
  {
    uint32_t rotated = r ? ((val << (2 * r)) | (val >> (32 - 2 * r))) : val;
    if (rotated <= 0xffu)
      return (w & 0xfffff000u) | (r << 8) | rotated;
  }
  return 0;
}

bool nam_elf_patch_registry(uint8_t* data, size_t data_len, const NamPair* pairs, uint32_t npairs, char* err,
                            size_t err_size)
{
  uint32_t D = 0;
  if (!nam_pairs_ids(pairs, npairs, &D, err, err_size))
    return false;
  const uint32_t R = npairs; /* extra picker rows */
  /* ---- 1. verify everything we are about to touch ---- */
  struct
  {
    uint32_t vaddr, want;
    const char* what;
  } checks[] = {
      {REG_VADDR_TYPES_START_LIT, REG_TYPES_START, "types start literal"},
      {REG_VADDR_TYPES_END_LIT, REG_TYPES_END, "types end literal"},
      {REG_NAMES_LIT_CTOR, REG_NAMES_BASE - 4, "ctor names literal"},
      {REG_NAMES_LIT_MAP, REG_NAMES_BASE - 4, "map names literal"},
      {REG_MAP_END_INSN, REG_MAP_END_INSN_OLD, "name-array end instruction"},
      {REG_CMP_LAST_IDX, REG_CMP_LAST_IDX_OLD, "ctor last-index compare"},
      {REG_CMP_COUNT, REG_CMP_COUNT_OLD, "ctor name-count compare"},
      {REG_MEMSET_SZ, REG_MEMSET_SZ_OLD, "ctor prototype-table memset size"},
      {REG_SHORT_LOOP_CMP, REG_SHORT_LOOP_CMP_OLD, "short-name loop bound"},
      {CAT_CALL_SITE, CAT_CALL_SITE_OLD, "picker catalog call site"},
      {CAT_VENEER_CASE_SLOT, CAT_VENEER, "dead case-84 jump table slot"},
  };
  for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i)
    if (!check_word(data, data_len, checks[i].vaddr, checks[i].want, checks[i].what, err, err_size))
      return false;
  for (size_t i = 0; i < sizeof(k_names16_lits) / sizeof(k_names16_lits[0]); ++i)
    if (!check_word(data, data_len, k_names16_lits[i], REG_NAMES_BASE + 16, "names+16 literal", err, err_size))
      return false;
  for (size_t i = 0; i < sizeof(k_id_sites) / sizeof(k_id_sites[0]); ++i)
  {
    if (!check_word(data, data_len, k_id_sites[i].vaddr, k_id_sites[i].old_w, "id-space constant", err, err_size))
      return false;
    uint32_t add = id_site_add(k_id_sites[i].kind, D, R);
    if (!bump_imm(k_id_sites[i].old_w, add))
    {
      set_err(err, err_size, "id-space constant @ 0x%x cannot be encoded for %u pairs", (unsigned)k_id_sites[i].vaddr,
              (unsigned)npairs);
      return false;
    }
  }
  if (REG_TYPES_END - REG_TYPES_START != REG_TYPE_COUNT * 4)
  {
    set_err(err, err_size, "internal error: type array size constants disagree");
    return false;
  }
  /* sanity: the name array's FX-Loop sits at 167 and every table's last normal entries are a pair */
  for (size_t t = 0; t < sizeof(k_id_tables) / sizeof(k_id_tables[0]); ++t)
  {
    uint32_t o = k_id_tables[t].base - VADDR_BASE;
    bool pair_ok = k_id_tables[t].distinct_pair ? rd32(data, o + 165 * 4) != rd32(data, o + 166 * 4)
                                                : rd32(data, o + 165 * 4) == rd32(data, o + 166 * 4);
    if ((size_t)o + REG_TABLE_BYTES + 4 * D > data_len || !pair_ok
        || rd32(data, o + 167 * 4) == rd32(data, o + 166 * 4))
    {
      set_err(err, err_size, "REFUSING: %s @ 0x%x does not have the expected pair layout (already patched?).",
              k_id_tables[t].what, (unsigned)k_id_tables[t].base);
      return false;
    }
  }
  for (uint32_t p = 0; p < npairs; ++p)
    if (strlen(pairs[p].name) > 12 || strlen(pairs[p].short_name) > 12)
    {
      set_err(err, err_size, "pedal names are limited to 12 characters (\" 2\" is appended)");
      return false;
    }

  /* ---- 2. allocate ---- */
  uint32_t str_bytes = 0, hook_names = 0;
  for (uint32_t p = 0; p < npairs; ++p)
  {
    size_t name_len = strlen(pairs[p].name), short_len = strlen(pairs[p].short_name);
    uint32_t inst = nam_pair_instances(&pairs[p]);
    str_bytes += (uint32_t)((name_len + 1) + (inst - 1) * (name_len + 3) + (short_len + 1) + (inst - 1) * (short_len + 3));
    hook_names += (uint32_t)((name_len + 1 + 3) & ~3u);
  }
  uint32_t types_words = 1 + REG_TYPE_COUNT + D;
  uint32_t cave_tables = 0, rw_tables = 0;
  for (size_t t = 0; t < sizeof(k_id_tables) / sizeof(k_id_tables[0]); ++t)
  {
    cave_tables += k_id_tables[t].place == PLACE_CAVE;
    rw_tables += k_id_tables[t].place == PLACE_RW_TAIL;
  }
  const uint32_t new_tab_bytes = (REG_NAME_COUNT + D) * 4;
  uint32_t hook_bytes = (uint32_t)sizeof(k_cat_hook) + 4u + 4u * npairs + hook_names;
  uint32_t cave_total = types_words * 4 + 2 * new_tab_bytes + cave_tables * new_tab_bytes + str_bytes + hook_bytes;
  uint32_t cave_off, cave_vaddr, rw_off = 0, rw_vaddr = 0;
  if (!cave_alloc(data, data_len, cave_total, &cave_off, &cave_vaddr, err, err_size))
    return false;
  if (rw_tables && !rw_tail_alloc(data, data_len, rw_tables * new_tab_bytes, &rw_off, &rw_vaddr, err, err_size))
    return false;

  uint32_t o = cave_off, v = cave_vaddr;
  uint32_t t_off = o, t_vaddr = v;
  o += types_words * 4;
  v += types_words * 4;
  uint32_t n_off = o, n_vaddr = v;
  o += new_tab_bytes;
  v += new_tab_bytes;
  uint32_t h_off = o, h_vaddr = v;
  o += new_tab_bytes;
  v += new_tab_bytes;
  uint32_t s_off = o, s_vaddr = v; /* strings go last, after the cave tables */
  o += str_bytes;
  v += str_bytes;
  o = (o + 3u) & ~3u;
  v = (v + 3u) & ~3u;

  /* ---- 3. strings: p_name[id] / p_short[id] for every extra id (names are "<n>", "<n> 2", "<n> 3", ...) ---- */
  uint32_t p_name[NAM_MAX_IDS], p_short[NAM_MAX_IDS];
  {
    uint32_t so = s_off, sv = s_vaddr, id = 0;
    for (uint32_t p = 0; p < npairs; ++p)
    {
      size_t nl = strlen(pairs[p].name), sl = strlen(pairs[p].short_name);
      uint32_t inst = nam_pair_instances(&pairs[p]);
      for (uint32_t k = 0; k < inst; ++k, ++id)
      {
        p_name[id] = sv;
        memcpy(data + so, pairs[p].name, nl);
        size_t add = k ? (size_t)snprintf((char*)data + so + nl, 4, " %u", (unsigned)(k + 1)) : 0;
        data[so + nl + add] = 0;
        so += (uint32_t)(nl + add + 1), sv += (uint32_t)(nl + add + 1);
        p_short[id] = sv;
        memcpy(data + so, pairs[p].short_name, sl);
        add = k ? (size_t)snprintf((char*)data + so + sl, 4, " %u", (unsigned)(k + 1)) : 0;
        data[so + sl + add] = 0;
        so += (uint32_t)(sl + add + 1), sv += (uint32_t)(sl + add + 1);
      }
    }
  }

  /* ---- 4. types array: header word + 166 entries + the new pairs ---- */
  memcpy(data + t_off, data + (REG_TYPES_START - VADDR_BASE), (1 + REG_TYPE_COUNT) * 4);
  for (uint32_t p = 0, id = 0; p < npairs; ++p)
    for (uint32_t k = 0; k < nam_pair_instances(&pairs[p]); ++k, ++id)
      wr32(data, t_off + (1 + REG_TYPE_COUNT + id) * 4, pairs[p].type_idx);
  wr32(data, REG_VADDR_TYPES_START_LIT - VADDR_BASE, t_vaddr);
  wr32(data, REG_VADDR_TYPES_END_LIT - VADDR_BASE, t_vaddr + (REG_TYPE_COUNT + D) * 4);

  /* ---- 5. names + short names: [0..166], new pairs, then FX-Loop last ---- */
  const uint32_t dst[2] = {n_off, h_off}, srcv[2] = {REG_NAMES_BASE, REG_SHORT_BASE};
  for (int i = 0; i < 2; ++i)
  {
    uint32_t src = srcv[i] - VADDR_BASE;
    for (uint32_t k = 0; k < REG_NAME_COUNT - 1; ++k)
      wr32(data, dst[i] + k * 4, rd32(data, src + k * 4));
    for (uint32_t id = 0; id < D; ++id)
      wr32(data, dst[i] + (REG_NAME_COUNT - 1 + id) * 4, i == 0 ? p_name[id] : p_short[id]);
    wr32(data, dst[i] + (REG_NAME_COUNT - 1 + D) * 4, rd32(data, src + (REG_NAME_COUNT - 1) * 4));
  }
  wr32(data, REG_NAMES_LIT_CTOR - VADDR_BASE, n_vaddr - 4);
  wr32(data, REG_NAMES_LIT_MAP - VADDR_BASE, n_vaddr - 4);
  for (size_t i = 0; i < sizeof(k_names16_lits) / sizeof(k_names16_lits[0]); ++i)
    wr32(data, k_names16_lits[i] - VADDR_BASE, n_vaddr + 16);
  for (size_t i = 0; i < sizeof(k_names_movw_sites) / sizeof(k_names_movw_sites[0]); ++i)
    if (!patch_movw_movt(data, data_len, k_names_movw_sites[i], REG_NAMES_BASE, n_vaddr, err, err_size))
      return false;
  if (!patch_movw_movt(data, data_len, REG_SHORT_MOVW, REG_SHORT_BASE, h_vaddr, err, err_size))
    return false;

  /* ---- 6. the other id-indexed tables. Cave/RW tables first, then the in-place ones (which overwrite the first
   *         entries of the old name / short-name arrays, already copied above). ---- */
  uint32_t next_cave_off = (s_off + str_bytes + 3u) & ~3u, next_cave_vaddr = (s_vaddr + str_bytes + 3u) & ~3u;
  uint32_t next_rw_off = rw_off, next_rw_vaddr = rw_vaddr;
  for (int pass = 0; pass < 2; ++pass)
  {
    for (size_t t = 0; t < sizeof(k_id_tables) / sizeof(k_id_tables[0]); ++t)
    {
      const IdTable* T = &k_id_tables[t];
      if ((pass == 0) == (T->place == PLACE_IN_PLACE))
        continue;
      uint32_t old_off = T->base - VADDR_BASE;
      if (T->place == PLACE_IN_PLACE)
      {
        extend_table(data, old_off, old_off, D);
        continue;
      }
      uint32_t d_off, d_vaddr;
      if (T->place == PLACE_CAVE)
      {
        d_off = next_cave_off, d_vaddr = next_cave_vaddr;
        next_cave_off += new_tab_bytes, next_cave_vaddr += new_tab_bytes;
      }
      else
      {
        d_off = next_rw_off, d_vaddr = next_rw_vaddr;
        next_rw_off += new_tab_bytes, next_rw_vaddr += new_tab_bytes;
      }
      extend_table(data, old_off, d_off, D);
      for (int s = 0; s < 2 && T->sites[s]; ++s)
        if (!patch_movw_movt(data, data_len, T->sites[s], T->base, d_vaddr, err, err_size))
          return false;
    }
  }

  /* ---- 6b. picker catalog: append the pair names to the Distortion list ---- */
  {
    uint32_t h_o = next_cave_off, h_v = next_cave_vaddr;
    memcpy(data + h_o, k_cat_hook, sizeof(k_cat_hook));
    wr32(data, h_o + CAT_HOOK_LIT_OFF, CAT_STR_RANGE_CTOR);
    wr32(data, h_o + CAT_HOOK_LIT_OFF + 4, CAT_STR_CSTR_CTOR);
    wr32(data, h_o + CAT_HOOK_LIT_OFF + 8, CAT_VEC_BUILD);
    uint32_t l_o = h_o + (uint32_t)sizeof(k_cat_hook), l_v = h_v + (uint32_t)sizeof(k_cat_hook);
    uint32_t n_o = l_o + 4 + 4 * npairs, n_v = l_v + 4 + 4 * npairs;
    wr32(data, l_o, npairs); /* extra names */
    for (uint32_t p = 0; p < npairs; ++p)
    {
      size_t nl = strlen(pairs[p].name);
      wr32(data, l_o + 4 + 4 * p, n_v);
      memcpy(data + n_o, pairs[p].name, nl + 1);
      uint32_t adv = (uint32_t)((nl + 1 + 3) & ~3u);
      n_o += adv, n_v += adv;
    }
    /* veneer: ldr r3,[pc] (list) ; ldr pc,[pc] (hook) ; .word list ; .word hook */
    uint32_t vo = CAT_VENEER - VADDR_BASE;
    wr32(data, vo + 0, 0xe59f3000u);
    wr32(data, vo + 4, 0xe59ff000u);
    wr32(data, vo + 8, l_v);
    wr32(data, vo + 12, h_v);
    int32_t rel = ((int32_t)CAT_VENEER - (int32_t)(CAT_CALL_SITE + 8)) >> 2;
    wr32(data, CAT_CALL_SITE - VADDR_BASE, 0xeb000000u | ((uint32_t)rel & 0xffffffu));
  }

  /* ---- 7. bounds and hard-coded ids ---- */
  struct
  {
    uint32_t vaddr, old_w, add;
  } bumps[] = {
      {REG_MAP_END_INSN, REG_MAP_END_INSN_OLD, 4 * D},
      {REG_SHORT_LOOP_CMP, REG_SHORT_LOOP_CMP_OLD, D},
      {REG_CMP_LAST_IDX, REG_CMP_LAST_IDX_OLD, D},
      {REG_CMP_COUNT, REG_CMP_COUNT_OLD, D},
      {REG_MEMSET_SZ, REG_MEMSET_SZ_OLD, 4 * D},
  };
  for (size_t i = 0; i < sizeof(bumps) / sizeof(bumps[0]); ++i)
  {
    uint32_t w = bump_imm(bumps[i].old_w, bumps[i].add);
    if (!w)
    {
      set_err(err, err_size, "bound @ 0x%x cannot be encoded for %u pairs", (unsigned)bumps[i].vaddr, (unsigned)npairs);
      return false;
    }
    wr32(data, bumps[i].vaddr - VADDR_BASE, w);
  }
  for (size_t i = 0; i < sizeof(k_id_sites) / sizeof(k_id_sites[0]); ++i)
  {
    uint32_t add = id_site_add(k_id_sites[i].kind, D, R);
    wr32(data, k_id_sites[i].vaddr - VADDR_BASE, bump_imm(k_id_sites[i].old_w, add));
  }
  return true;
}

bool nam_elf_patch_registry_test(uint8_t* data, size_t data_len, const char* name, const char* short_name,
                                 uint32_t type_idx, char* err, size_t err_size)
{
  NamPair p = {name, short_name, type_idx};
  return nam_elf_patch_registry(data, data_len, &p, 1, err, err_size);
}

/* ---- property-node registration for the extra pairs ----------------------------------------------------------
 *
 * The StateMap builder (fcn 0x5f88a8) creates every pedal's property node by code: FUN_00594ab4(this, name, storage,
 * descriptors, nParams, id), later FUN_005938bc(storage, descriptors, "/Engine/MixerCtrl/", "/Engine/MixerCtrl2/",
 * "<Name>/") to link them to the engine's parameters, and finally FUN_00406014(this, a, b) which walks a hard-coded
 * list of every pedal's storage (FUN_005b3654) to connect each property to its value source. A pair added only
 * through the name tables has none of this: no node (crash on save), then no links, then no value source (a
 * transparent pedal with dead knobs). Three hooks fix that for ids 167.. with Glorious Drive's descriptors (the pairs
 * are built as that type):
 *  A - the Glorious Drive 2 node call (0x5fd3b8) runs the original, then registers every extra instance;
 *  B - the Glorious Drive path-link call (0x6041fc) runs the original, then links every extra instance;
 *  C - the entry of 0x406014 (two displaced instructions) activates the extra storages first.
 * A and B are reached through veneers in dead code (the calls are out of bl range of the cave); the hooks themselves
 * live in the dead original types array. Per instance (storage, name, path name) triples follow the hook code; the
 * storages are private zeroed 12-byte vectors in the R+W tail. */
#define NODE_CALL_A 0x5fd3b8u /* bl 0x594ab4, Glorious Drive 2 */
#define NODE_CALL_A_W 0xebfe5dbdu
#define NODE_CALL_B 0x6041fcu /* bl 0x5938bc, Glorious Drive */
#define NODE_CALL_B_W 0xebfe3daeu
#define NODE_VENEER_A 0x209e38u
#define NODE_VENEER_B 0x209e40u
#define NODE_VENEER_A_W 0xe1a0b000u /* mov fp, r0 (dead case-84 body) */
#define NODE_VENEER_B_W 0xeb0a1bc4u
#define ENG_TAB_BYTES (4u + 8u * 8u) /* engine table: count + 8 x {engine ptr, name QString ptr} */
#define NODE_HOOK_B_OFF 0xa8u
#define NODE_HOOK_C_OFF 0x164u
#define NODE_HOOK_D_OFF 0x1e8u
#define NODE_HOOK_TAB_OFF 0x244u

static const uint8_t k_node_hooks[616] = {
  0xf0, 0x47, 0x2d, 0xe9, 0x00, 0x80, 0xa0, 0xe1, 0x20, 0x40, 0x9d, 0xe5,
  0x24, 0x50, 0x9d, 0xe5, 0x08, 0xd0, 0x4d, 0xe2, 0x00, 0x40, 0x8d, 0xe5,
  0x04, 0x50, 0x8d, 0xe5, 0x88, 0x9f, 0x8f, 0xe2, 0x00, 0xc0, 0x99, 0xe5,
  0x3c, 0xff, 0x2f, 0xe1, 0x08, 0xd0, 0x8d, 0xe2, 0x20, 0xd0, 0x4d, 0xe2,
  0x08, 0x60, 0x8d, 0xe2, 0x24, 0xa0, 0x89, 0xe2, 0x00, 0x70, 0xa0, 0xe3,
  0x08, 0x00, 0x86, 0xe2, 0x00, 0x00, 0x86, 0xe5, 0x06, 0x00, 0xa0, 0xe1,
  0x04, 0x10, 0x9a, 0xe5, 0x04, 0xc0, 0x99, 0xe5, 0x3c, 0xff, 0x2f, 0xe1,
  0x00, 0x20, 0x9a, 0xe5, 0x00, 0x30, 0xa0, 0xe3, 0x00, 0x30, 0x82, 0xe5,
  0x04, 0x30, 0x82, 0xe5, 0x08, 0x30, 0x82, 0xe5, 0x08, 0x00, 0xa0, 0xe1,
  0x06, 0x10, 0xa0, 0xe1, 0x0c, 0x30, 0x99, 0xe5, 0x07, 0x40, 0xa0, 0xe3,
  0x00, 0x40, 0x8d, 0xe5, 0xa7, 0x40, 0x87, 0xe2, 0x04, 0x40, 0x8d, 0xe5,
  0x00, 0xc0, 0x99, 0xe5, 0x3c, 0xff, 0x2f, 0xe1, 0x0c, 0xa0, 0x8a, 0xe2,
  0x01, 0x70, 0x87, 0xe2, 0x14, 0x30, 0x99, 0xe5, 0x03, 0x00, 0x57, 0xe1,
  0xe6, 0xff, 0xff, 0xba, 0x20, 0xd0, 0x8d, 0xe2, 0xf0, 0x87, 0xbd, 0xe8,
  0xf0, 0x47, 0x2d, 0xe9, 0x20, 0x50, 0x9d, 0xe5, 0x08, 0xd0, 0x4d, 0xe2,
  0x00, 0x50, 0x8d, 0xe5, 0x61, 0x9f, 0x8f, 0xe2, 0x08, 0xc0, 0x99, 0xe5,
  0x3c, 0xff, 0x2f, 0xe1, 0x08, 0xd0, 0x8d, 0xe2, 0x50, 0xd0, 0x4d, 0xe2,
  0x24, 0xa0, 0x89, 0xe2, 0x00, 0x70, 0xa0, 0xe3, 0x08, 0x60, 0x8d, 0xe2,
  0x08, 0x00, 0x86, 0xe2, 0x00, 0x00, 0x86, 0xe5, 0x06, 0x00, 0xa0, 0xe1,
  0x18, 0x10, 0x99, 0xe5, 0x04, 0xc0, 0x99, 0xe5, 0x3c, 0xff, 0x2f, 0xe1,
  0x20, 0x60, 0x8d, 0xe2, 0x08, 0x00, 0x86, 0xe2, 0x00, 0x00, 0x86, 0xe5,
  0x06, 0x00, 0xa0, 0xe1, 0x1c, 0x10, 0x99, 0xe5, 0x04, 0xc0, 0x99, 0xe5,
  0x3c, 0xff, 0x2f, 0xe1, 0x38, 0x60, 0x8d, 0xe2, 0x08, 0x00, 0x86, 0xe2,
  0x00, 0x00, 0x86, 0xe5, 0x06, 0x00, 0xa0, 0xe1, 0x08, 0x10, 0x9a, 0xe5,
  0x04, 0xc0, 0x99, 0xe5, 0x3c, 0xff, 0x2f, 0xe1, 0x00, 0x00, 0x9a, 0xe5,
  0x0c, 0x10, 0x99, 0xe5, 0x08, 0x20, 0x8d, 0xe2, 0x20, 0x30, 0x8d, 0xe2,
  0x38, 0x40, 0x8d, 0xe2, 0x00, 0x40, 0x8d, 0xe5, 0x08, 0xc0, 0x99, 0xe5,
  0x3c, 0xff, 0x2f, 0xe1, 0x0c, 0xa0, 0x8a, 0xe2, 0x01, 0x70, 0x87, 0xe2,
  0x14, 0x30, 0x99, 0xe5, 0x03, 0x00, 0x57, 0xe1, 0xdd, 0xff, 0xff, 0xba,
  0x50, 0xd0, 0x8d, 0xe2, 0xf0, 0x87, 0xbd, 0xe8, 0xf7, 0x40, 0x2d, 0xe9,
  0x01, 0x40, 0xa0, 0xe1, 0x02, 0x50, 0xa0, 0xe1, 0x33, 0x6f, 0x8f, 0xe2,
  0x24, 0x30, 0x86, 0xe2, 0x00, 0x70, 0xa0, 0xe3, 0x00, 0x00, 0x93, 0xe5,
  0x04, 0x10, 0xa0, 0xe1, 0x05, 0x20, 0xa0, 0xe1, 0x10, 0xc0, 0x96, 0xe5,
  0x04, 0x30, 0x2d, 0xe5, 0x04, 0xd0, 0x4d, 0xe2, 0x3c, 0xff, 0x2f, 0xe1,
  0x04, 0xd0, 0x8d, 0xe2, 0x04, 0x30, 0x9d, 0xe4, 0x0c, 0x30, 0x83, 0xe2,
  0x01, 0x70, 0x87, 0xe2, 0x14, 0x00, 0x96, 0xe5, 0x00, 0x00, 0x57, 0xe1,
  0xf1, 0xff, 0xff, 0xba, 0x20, 0x00, 0x96, 0xe5, 0x00, 0xc0, 0x90, 0xe5,
  0x00, 0x00, 0x5c, 0xe3, 0x03, 0x00, 0x00, 0x0a, 0x24, 0x00, 0x86, 0xe2,
  0x14, 0x10, 0x96, 0xe5, 0x00, 0x20, 0x9d, 0xe5, 0x3c, 0xff, 0x2f, 0xe1,
  0xf7, 0x40, 0xbd, 0xe8, 0xf8, 0x41, 0x6d, 0xe1, 0x01, 0x40, 0xa0, 0xe1,
  0x04, 0xf0, 0x1f, 0xe5, 0x1c, 0x60, 0x40, 0x00, 0xa7, 0x30, 0x46, 0xe2,
  0x03, 0x00, 0x53, 0xe3, 0x0d, 0x00, 0x00, 0x2a, 0x01, 0x60, 0x86, 0xe2,
  0x04, 0x00, 0xa0, 0xe1, 0x06, 0x10, 0xa0, 0xe1, 0x30, 0xc0, 0x9f, 0xe5,
  0x3c, 0xff, 0x2f, 0xe1, 0x00, 0x30, 0xe0, 0xe1, 0xa3, 0x3f, 0xa0, 0xe1,
  0x05, 0x00, 0x50, 0xe1, 0x00, 0x30, 0xa0, 0x03, 0x00, 0x00, 0x53, 0xe3,
  0x18, 0xf0, 0x9f, 0x05, 0xa7, 0x30, 0x46, 0xe2, 0x03, 0x00, 0x53, 0xe3,
  0xf1, 0xff, 0xff, 0x3a, 0x08, 0x10, 0xa0, 0xe1, 0x38, 0x00, 0x8d, 0xe2,
  0x04, 0xf0, 0x9f, 0xe5, 0x74, 0xce, 0x3c, 0x00, 0x44, 0xdd, 0x3c, 0x00,
  0xe8, 0xdf, 0x3c, 0x00, 0xb4, 0x4a, 0x59, 0x00, 0x84, 0x2e, 0x5b, 0x00,
  0xbc, 0x38, 0x59, 0x00, 0x44, 0xa2, 0x3c, 0x03, 0x54, 0x36, 0x5b, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
};

static bool g_rename_params;
void nam_spy_set_rename_params(bool on)
{
  g_rename_params = on;
}

/* file offset of a vaddr inside the plain R+W LOAD segment, or 0 */
static uint32_t rw_off_of(const uint8_t* data, uint32_t vaddr)
{
  uint32_t e_phoff = rd32(data, 28);
  uint16_t e_phentsize = (uint16_t)(data[42] | (data[43] << 8));
  uint16_t e_phnum = (uint16_t)(data[44] | (data[45] << 8));
  for (uint16_t i = 0; i < e_phnum; ++i)
  {
    size_t ph = (size_t)e_phoff + (size_t)i * e_phentsize;
    if (rd32(data, ph) == PT_LOAD && rd32(data, ph + 24) == (PF_R | PF_W))
    {
      uint32_t p_off = rd32(data, ph + 4), p_vaddr = rd32(data, ph + 8), filesz = rd32(data, ph + 16);
      if (vaddr >= p_vaddr && vaddr < p_vaddr + filesz)
        return vaddr - p_vaddr + p_off;
    }
  }
  return 0;
}

bool nam_elf_patch_node_registration(uint8_t* data, size_t data_len, const NamPair* pairs, uint32_t npairs,
                                     uint32_t* out_init_slot, uint32_t* out_eng_tab, char* err, size_t err_size)
{
  uint32_t inst = 0;
  if (!nam_pairs_ids(pairs, npairs, &inst, err, err_size))
    return false;
  if (!check_word(data, data_len, NODE_CALL_A, NODE_CALL_A_W, "Glorious Drive 2 node call", err, err_size)
      || !check_word(data, data_len, NODE_CALL_B, NODE_CALL_B_W, "Glorious Drive path call", err, err_size)
      || !check_word(data, data_len, NODE_VENEER_A, NODE_VENEER_A_W, "dead veneer A", err, err_size)
      || !check_word(data, data_len, NODE_VENEER_B, NODE_VENEER_B_W, "dead veneer B", err, err_size)
      || !check_word(data, data_len, 0x406014u, 0xe16d41f8u, "activate-all entry", err, err_size)
      || !check_word(data, data_len, 0x406018u, 0xe1a04001u, "activate-all entry+4", err, err_size))
    return false;
  const uint32_t region = (1u + REG_TYPE_COUNT) * 4u;
  const uint32_t blob_len = (uint32_t)sizeof(k_node_hooks);
  if (blob_len + 12u * inst > region)
  {
    set_err(err, err_size, "REFUSING: node hooks do not fit the old types array");
    return false;
  }
  if (rd32(data, REG_VADDR_TYPES_START_LIT - VADDR_BASE) == REG_TYPES_START)
  {
    set_err(err, err_size, "REFUSING: registry patch must run first (old types array still in use)");
    return false;
  }
  /* storages (writable) go in the R+W tail; the strings are read-only and fit the dead original name array, past the
   * entries the in-place category-table extension overwrites */
  static const char k_p1[] = "/Engine/MixerCtrl/", k_p2[] = "/Engine/MixerCtrl2/";
  uint32_t str_bytes = (uint32_t)(sizeof(k_p1) + sizeof(k_p2) + 8) + (g_rename_params ? 24u : 0u);
  for (uint32_t p = 0; p < npairs; ++p)
    str_bytes += nam_pair_instances(&pairs[p]) * (uint32_t)((strlen(pairs[p].name) + 8 + 3) & ~3u) * 2u;
  const uint32_t str_start = REG_NAMES_BASE + 4u * inst + 8u;
  if (str_start + str_bytes > REG_NAMES_BASE + 0xf8u /* the engine vtable clone starts at +0x100 */
      || rd32(data, REG_NAMES_LIT_CTOR - VADDR_BASE) == REG_NAMES_BASE - 4)
  {
    set_err(err, err_size, "REFUSING: node strings do not fit the old name array, or the registry patch has not run");
    return false;
  }
  uint32_t r_off, r_vaddr;
  /* storages, then the init-hook slot (one word, read by hook C) and the engine table the engine stub fills
   * ({count, 8 x {engine, &name}}) */
  if (!rw_tail_alloc(data, data_len, 12u * inst + 4u + ENG_TAB_BYTES, &r_off, &r_vaddr, err, err_size))
    return false;
  const uint32_t c_off = REG_TYPES_START - VADDR_BASE, c_vaddr = REG_TYPES_START;
  memset(data + c_off, 0, region);
  memcpy(data + c_off, k_node_hooks, blob_len);
  const uint32_t tab = c_off + NODE_HOOK_TAB_OFF;
  uint32_t so = str_start - VADDR_BASE, sv = str_start;
#define NODE_STR(text, out_v)                                                                                        \
  do                                                                                                                 \
  {                                                                                                                  \
    const char* s_ = (text);                                                                                         \
    size_t l_ = strlen(s_) + 1;                                                                                      \
    memcpy(data + so, s_, l_);                                                                                       \
    (out_v) = sv;                                                                                                    \
    so += (uint32_t)((l_ + 3u) & ~3u);                                                                               \
    sv += (uint32_t)((l_ + 3u) & ~3u);                                                                               \
  } while (0)
  uint32_t v_p1, v_p2;
  NODE_STR(k_p1, v_p1);
  NODE_STR(k_p2, v_p2);
  const uint32_t init_slot = r_vaddr + 12u * inst;
  if (g_rename_params)
  {
    /* own copy of Glorious Drive's 7-entry parameter-name table (Drive Level Tone Gain Hi-Cut On Colour) with the first
     * three renamed; hooks A and B pass the table through tab word 3 */
    const uint32_t gd_tab = 0x33ca244u;
    const uint32_t gd_off = rw_off_of(data, gd_tab);
    uint32_t t_off, t_vaddr, v_m, v_o, v_i;
    if (!gd_off || gd_off + 28 > data_len || !rw_tail_alloc(data, data_len, 28, &t_off, &t_vaddr, err, err_size))
    {
      if (!gd_off || gd_off + 28 > data_len)
        set_err(err, err_size, "REFUSING: Glorious Drive parameter-name table not found in the R+W segment");
      return false;
    }
    if (rd32(data, gd_off) != 0x15ad65cu /* "Drive" */ || rd32(data, gd_off + 4) != 0x15ad66cu /* "Level" */
        || rd32(data, gd_off + 8) != 0x15ad664u /* "Tone" */)
    {
      set_err(err, err_size, "REFUSING: Glorious Drive parameter-name table has unexpected contents");
      return false;
    }
    NODE_STR("Model", v_m);
    NODE_STR("OutVol", v_o);
    NODE_STR("InVol", v_i);
    memcpy(data + t_off, data + gd_off, 28);
    wr32(data, t_off, v_m);
    wr32(data, t_off + 4, v_o);
    wr32(data, t_off + 8, v_i);
    wr32(data, tab + 12, t_vaddr);
  }
  wr32(data, tab + 20, inst);
  wr32(data, tab + 32, init_slot);
  wr32(data, tab + 24, v_p1);
  wr32(data, tab + 28, v_p2);
  uint32_t i = 0;
  for (uint32_t p = 0; p < npairs; ++p)
    for (uint32_t k = 0; k < nam_pair_instances(&pairs[p]); ++k, ++i)
    {
      char nm[40], pn[48];
      if (k)
        snprintf(nm, sizeof(nm), "%s %u", pairs[p].name, (unsigned)(k + 1));
      else
        snprintf(nm, sizeof(nm), "%s", pairs[p].name);
      snprintf(pn, sizeof(pn), "%s/", nm);
      uint32_t v_name, v_path;
      NODE_STR(nm, v_name);
      NODE_STR(pn, v_path);
      uint32_t t = tab + 36 + 12 * i;
      wr32(data, t, r_vaddr + 12 * i);
      wr32(data, t + 4, v_name);
      wr32(data, t + 8, v_path);
    }
#undef NODE_STR
  /* hook D: a 4-instance pedal tries ids first+2 and first+3 when the picker finds the first two in use. The
   * displaced pair at 0x3cdfe0 is the picker's "both instances in use" exit. */
  if (inst == 4)
  {
    if (!check_word(data, data_len, 0x3cdfe0u, 0xe1a01008u, "picker exit", err, err_size)
        || !check_word(data, data_len, 0x3cdfe4u, 0xe28d0038u, "picker exit+4", err, err_size))
      return false;
    wr32(data, 0x3cdfe0u - VADDR_BASE, 0xe51ff004u);
    wr32(data, 0x3cdfe4u - VADDR_BASE, c_vaddr + NODE_HOOK_D_OFF);
  }
  /* hook C: displaced entry of 0x406014 */
  wr32(data, 0x406014u - VADDR_BASE, 0xe51ff004u);
  wr32(data, 0x406018u - VADDR_BASE, c_vaddr + NODE_HOOK_C_OFF);
  /* veneers A/B: ldr pc,[pc,#-4] ; .word hook */
  wr32(data, NODE_VENEER_A - VADDR_BASE, 0xe51ff004u);
  wr32(data, NODE_VENEER_A - VADDR_BASE + 4, c_vaddr);
  wr32(data, NODE_VENEER_B - VADDR_BASE, 0xe51ff004u);
  wr32(data, NODE_VENEER_B - VADDR_BASE + 4, c_vaddr + NODE_HOOK_B_OFF);
  wr32(data, NODE_CALL_A - VADDR_BASE,
       0xeb000000u | ((uint32_t)(((int32_t)NODE_VENEER_A - (int32_t)(NODE_CALL_A + 8)) >> 2) & 0xffffffu));
  wr32(data, NODE_CALL_B - VADDR_BASE,
       0xeb000000u | ((uint32_t)(((int32_t)NODE_VENEER_B - (int32_t)(NODE_CALL_B + 8)) >> 2) & 0xffffffu));
  if (out_init_slot)
    *out_init_slot = init_slot;
  if (out_eng_tab)
    *out_eng_tab = init_slot + 4u;
  return true;
}

/* ---- own audio engine for the extra pedals ------------------------------------------------------------------
 *
 * Extra pedals built as Glorious Drive would share that pedal's engine class (vtable 0x1828f44, process() slot 8 =
 * 0x2c6b68). To run NAM only in the extra pedals, they get their own copies of both vtables:
 *  - ModFac_construct's dead case for type 80 is turned into a Glorious-Drive-shaped constructor (its two literals are
 *    repointed: VTT 0x182314c, module vtable = a clone of 0x1824588), so the registry builds the extra pedals as
 *    type 80;
 *  - the module vtable clone's setup slot (19, 0x27cc20) points at a stub that runs the original setup (which creates
 *    the engine at module+0x270) and then repoints the engine's vtable at a clone of 0x1828f44 whose process() slot
 *    is a trampoline: it jumps through a writable hook slot that libnam_preload.so fills in at run time, else falls
 *    back to the real process().
 * The vtable clones and the stub live in the dead original short-name array (the module side) and name array (the
 * engine side); the hook slot is one word of the R+W tail. */
#define ENG_TYPE 80u
#define ENG_CASE_VTT_LIT 0x20aa80u /* dead case 80's literals */
#define ENG_CASE_VTT_LIT_OLD 0x182362cu
#define ENG_CASE_VT_LIT 0x20aa84u
#define ENG_CASE_VT_LIT_OLD 0x1825b78u
#define ENG_GD_VTT_LIT 0x20aa48u /* Glorious Drive's case-91 literals */
#define ENG_GD_VT_LIT 0x20aa4cu
#define ENG_MOD_VT 0x1824588u
#define ENG_MOD_VT_WORDS 64u
#define ENG_SETUP_SLOT 19u
#define ENG_SETUP_FN 0x27cc20u
#define ENG_VT 0x1828f44u
#define ENG_VT_WORDS 72u
#define ENG_PROCESS_SLOT 8u
#define ENG_PROCESS_FN 0x2c6b68u
#define ENG_VT_LIT 0x27d0b8u /* setup's engine-vtable literal (cross-check only) */
#define ENG_STUB_SETUP_OFF 0x00u
#define ENG_STUB_TRAMP_OFF 0x6cu
#define ENG_STUB_LIT_EVT_OFF 0x84u
#define ENG_STUB_LIT_SLOT_OFF 0x88u
#define ENG_STUB_LIT_TAB_OFF 0x90u

static const uint8_t k_eng_stub[148] = {
  0x70, 0x40, 0x2d, 0xe9, 0x10, 0xc0, 0x9d, 0xe5, 0x08, 0xd0, 0x4d, 0xe2,
  0x00, 0xc0, 0x8d, 0xe5, 0x00, 0x40, 0xa0, 0xe1, 0x02, 0x60, 0xa0, 0xe1,
  0x60, 0xc0, 0x9f, 0xe5, 0x3c, 0xff, 0x2f, 0xe1, 0x08, 0xd0, 0x8d, 0xe2,
  0x00, 0x50, 0xa0, 0xe1, 0x70, 0x02, 0x94, 0xe5, 0x00, 0x00, 0x50, 0xe3,
  0x0b, 0x00, 0x00, 0x0a, 0x48, 0x10, 0x9f, 0xe5, 0x00, 0x10, 0x80, 0xe5,
  0x4c, 0x10, 0x9f, 0xe5, 0x00, 0x20, 0x91, 0xe5, 0x08, 0x00, 0x52, 0xe3,
  0x05, 0x00, 0x00, 0x2a, 0x04, 0x30, 0x81, 0xe2, 0x82, 0x31, 0x83, 0xe0,
  0x00, 0x00, 0x83, 0xe5, 0x04, 0x60, 0x83, 0xe5, 0x01, 0x20, 0x82, 0xe2,
  0x00, 0x20, 0x81, 0xe5, 0x05, 0x00, 0xa0, 0xe1, 0x70, 0x80, 0xbd, 0xe8,
  0x14, 0xc0, 0x9f, 0xe5, 0x00, 0xc0, 0x9c, 0xe5, 0x00, 0x00, 0x5c, 0xe3,
  0x1c, 0xff, 0x2f, 0x11, 0x08, 0xf0, 0x9f, 0xe5, 0x20, 0xcc, 0x27, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x6b, 0x2c, 0x00,
  0x00, 0x00, 0x00, 0x00,
};

bool nam_elf_patch_engine_class(uint8_t* data, size_t data_len, uint32_t ids, uint32_t eng_tab, uint32_t* out_hook_slot,
                                char* err, size_t err_size)
{
  if (!check_word(data, data_len, ENG_CASE_VTT_LIT, ENG_CASE_VTT_LIT_OLD, "dead case 80 VTT literal", err, err_size)
      || !check_word(data, data_len, ENG_CASE_VT_LIT, ENG_CASE_VT_LIT_OLD, "dead case 80 vtable literal", err, err_size)
      || !check_word(data, data_len, ENG_GD_VT_LIT, ENG_MOD_VT, "Glorious Drive module vtable literal", err, err_size)
      || !check_word(data, data_len, ENG_VT_LIT, ENG_VT, "engine vtable literal", err, err_size)
      || !check_word(data, data_len, ENG_MOD_VT + 4 * ENG_SETUP_SLOT, ENG_SETUP_FN, "module setup slot", err, err_size)
      || !check_word(data, data_len, ENG_VT + 4 * ENG_PROCESS_SLOT, ENG_PROCESS_FN, "engine process slot", err,
                     err_size))
    return false;
  /* the registry patch must have run (it moves the old short-name / name arrays out of use) */
  if (rd32(data, REG_NAMES_LIT_CTOR - VADDR_BASE) == REG_NAMES_BASE - 4)
  {
    set_err(err, err_size, "REFUSING: registry patch must run first (old name arrays still in use)");
    return false;
  }
  /* module side: short-name array past the in-place P1 extension */
  const uint32_t mod_hdr = (REG_SHORT_BASE + 4u * ids + 8u + 7u) & ~7u; /* header words, 8-aligned */
  const uint32_t mod_vt = mod_hdr + 8u;
  const uint32_t stub = mod_vt + 4u * ENG_MOD_VT_WORDS;
  const uint32_t stub_end = stub + (uint32_t)sizeof(k_eng_stub);
  if (stub_end > REG_SHORT_BASE + REG_NAME_COUNT * 4u)
  {
    set_err(err, err_size, "REFUSING: engine stubs do not fit the old short-name array");
    return false;
  }
  /* engine side: name array, clear of the node strings (which start at +4*ids+8) */
  const uint32_t eng_hdr = REG_NAMES_BASE + 0x100u;
  const uint32_t eng_vt = eng_hdr + 8u;
  if (eng_vt + 4u * ENG_VT_WORDS > REG_NAMES_BASE + REG_NAME_COUNT * 4u)
  {
    set_err(err, err_size, "REFUSING: engine vtable does not fit the old name array");
    return false;
  }
  uint32_t slot_off, slot_vaddr;
  if (!rw_tail_alloc(data, data_len, 4, &slot_off, &slot_vaddr, err, err_size))
    return false;

  /* vtable clones: [offset-to-top, typeinfo] + entries */
  memcpy(data + (mod_hdr - VADDR_BASE), data + (ENG_MOD_VT - 8 - VADDR_BASE), 8u + 4u * ENG_MOD_VT_WORDS);
  memcpy(data + (eng_hdr - VADDR_BASE), data + (ENG_VT - 8 - VADDR_BASE), 8u + 4u * ENG_VT_WORDS);
  /* stub + trampoline */
  memcpy(data + (stub - VADDR_BASE), k_eng_stub, sizeof(k_eng_stub));
  wr32(data, stub - VADDR_BASE + ENG_STUB_LIT_EVT_OFF, eng_vt);
  wr32(data, stub - VADDR_BASE + ENG_STUB_LIT_SLOT_OFF, slot_vaddr);
  wr32(data, stub - VADDR_BASE + ENG_STUB_LIT_TAB_OFF, eng_tab);
  wr32(data, mod_vt - VADDR_BASE + 4u * ENG_SETUP_SLOT, stub + ENG_STUB_SETUP_OFF);
  wr32(data, eng_vt - VADDR_BASE + 4u * ENG_PROCESS_SLOT, stub + ENG_STUB_TRAMP_OFF);
  /* dead case 80 builds a Glorious-Drive-shaped module with the cloned vtable */
  wr32(data, ENG_CASE_VTT_LIT - VADDR_BASE, rd32(data, ENG_GD_VTT_LIT - VADDR_BASE));
  wr32(data, ENG_CASE_VT_LIT - VADDR_BASE, mod_vt);
  *out_hook_slot = slot_vaddr;
  return true;
}

/* ---- metacall spy -------------------------------------------------------------------------------------------
 *
 * fcn 0x8686c..0x87d7c is a qt_static_metacall-style dispatcher: `cmp r2,#0x30; ldrls pc,[pc,r2,lsl 2]` with the
 * (absolute) case addresses in a 49-word table at 0x867a0. Each non-trivial table word is redirected to a 20-byte
 * stub that saves r0-r3/ip/lr, loads the method index and the original target, and branches to a shared logger. The
 * logger appends {0xa5000000|index, _a, _a[1], _a[2], this, caller lr} (24 bytes) plus up to 8 bytes read from each
 * of _a[1] and _a[2] to /tmp/meta.bin using writev (a bad pointer makes writev return a short count instead of
 * faulting), stops at 1MB, restores every register and jumps to the original case through lr (dead at a case
 * entry: every case leaves through an epilogue that pops its saved lr). */
#define META_DISPATCH_CMP 0x86794u /* cmp r2, #0x30 */
#define META_DISPATCH_CMP_W 0xe3520030u
#define META_DISPATCH_JMP 0x86798u /* ldrls pc, [pc, r2, lsl 2] */
#define META_DISPATCH_JMP_W 0x979ff102u
#define META_TABLE 0x867a0u
#define META_ENTRIES 0x31u
#define META_EPILOGUE 0x8686cu /* table words pointing here are no-ops, left alone */
#define META_STUB_BYTES 20u

static const uint8_t k_meta_common[248] = {
  0xf0, 0x00, 0x2d, 0xe9, 0xa5, 0x04, 0x80, 0xe3, 0x40, 0xd0, 0x4d, 0xe2,
  0x5c, 0x50, 0x9d, 0xe5, 0x01, 0x60, 0xa0, 0xe1, 0x50, 0x40, 0x9d, 0xe5,
  0x00, 0x10, 0xa0, 0xe3, 0x00, 0x20, 0xa0, 0xe3, 0x01, 0x08, 0x55, 0xe3,
  0x03, 0x00, 0x00, 0x3a, 0x03, 0x00, 0x15, 0xe3, 0x01, 0x00, 0x00, 0x1a,
  0x04, 0x10, 0x95, 0xe5, 0x08, 0x20, 0x95, 0xe5, 0x64, 0x30, 0x9d, 0xe5,
  0x00, 0x00, 0x8d, 0xe5, 0x04, 0x50, 0x8d, 0xe5, 0x08, 0x10, 0x8d, 0xe5,
  0x0c, 0x20, 0x8d, 0xe5, 0x10, 0x40, 0x8d, 0xe5, 0x14, 0x30, 0x8d, 0xe5,
  0x0d, 0x30, 0xa0, 0xe1, 0x18, 0x30, 0x8d, 0xe5, 0x18, 0x30, 0xa0, 0xe3,
  0x1c, 0x30, 0x8d, 0xe5, 0x20, 0x10, 0x8d, 0xe5, 0x08, 0x30, 0xa0, 0xe3,
  0x24, 0x30, 0x8d, 0xe5, 0x28, 0x20, 0x8d, 0xe5, 0x2c, 0x30, 0x8d, 0xe5,
  0x68, 0x00, 0x8f, 0xe2, 0x41, 0x14, 0x00, 0xe3, 0x69, 0x2f, 0xa0, 0xe3,
  0x05, 0x70, 0xa0, 0xe3, 0x00, 0x00, 0x00, 0xef, 0x00, 0x40, 0xb0, 0xe1,
  0x0e, 0x00, 0x00, 0x4a, 0x04, 0x00, 0xa0, 0xe1, 0x00, 0x10, 0xa0, 0xe3,
  0x02, 0x20, 0xa0, 0xe3, 0x13, 0x70, 0xa0, 0xe3, 0x00, 0x00, 0x00, 0xef,
  0x01, 0x06, 0x50, 0xe3, 0x04, 0x00, 0x00, 0x2a, 0x04, 0x00, 0xa0, 0xe1,
  0x18, 0x10, 0x8d, 0xe2, 0x03, 0x20, 0xa0, 0xe3, 0x92, 0x70, 0xa0, 0xe3,
  0x00, 0x00, 0x00, 0xef, 0x04, 0x00, 0xa0, 0xe1, 0x06, 0x70, 0xa0, 0xe3,
  0x00, 0x00, 0x00, 0xef, 0x06, 0xe0, 0xa0, 0xe1, 0x40, 0xd0, 0x8d, 0xe2,
  0xf0, 0x00, 0xbd, 0xe8, 0x0f, 0x10, 0xbd, 0xe8, 0x04, 0xd0, 0x8d, 0xe2,
  0x1e, 0xff, 0x2f, 0xe1, 0x2f, 0x74, 0x6d, 0x70, 0x2f, 0x6d, 0x65, 0x74,
  0x61, 0x2e, 0x62, 0x69, 0x6e, 0x00, 0x00, 0x00,
};

bool nam_elf_patch_metacall_spy(uint8_t* data, size_t data_len, uint64_t method_mask, char* err, size_t err_size)
{
  if (!check_word(data, data_len, META_DISPATCH_CMP, META_DISPATCH_CMP_W, "metacall dispatch compare", err, err_size)
      || !check_word(data, data_len, META_DISPATCH_JMP, META_DISPATCH_JMP_W, "metacall dispatch jump", err, err_size))
    return false;

  uint32_t targets[META_ENTRIES];
  uint32_t n_stubs = 0;
  for (uint32_t i = 0; i < META_ENTRIES; ++i)
  {
    uint32_t t = rd32(data, META_TABLE - VADDR_BASE + i * 4);
    if (t < 0x86800u || t >= 0x88000u)
    {
      set_err(err, err_size, "REFUSING: metacall table entry %u holds 0x%x (not a case address; already patched?)", i,
              (unsigned)t);
      return false;
    }
    targets[i] = t;
    n_stubs += (t != META_EPILOGUE) && ((method_mask >> i) & 1u);
  }

  uint32_t need = (uint32_t)sizeof(k_meta_common) + n_stubs * META_STUB_BYTES;
  uint32_t off, vaddr;
  if (!cave_alloc(data, data_len, need, &off, &vaddr, err, err_size))
    return false;
  memcpy(data + off, k_meta_common, sizeof(k_meta_common));
  uint32_t common = vaddr;
  uint32_t so = off + (uint32_t)sizeof(k_meta_common), sv = vaddr + (uint32_t)sizeof(k_meta_common);
  for (uint32_t i = 0; i < META_ENTRIES; ++i)
  {
    if (targets[i] == META_EPILOGUE || !((method_mask >> i) & 1u))
      continue;
    int32_t rel = ((int32_t)common - (int32_t)(sv + 12 + 8)) >> 2;
    wr32(data, so + 0, 0xe92d500fu);                        /* push {r0-r3, ip, lr} */
    wr32(data, so + 4, 0xe3a00000u | i);                    /* mov r0, #i           */
    wr32(data, so + 8, 0xe59f1000u);                        /* ldr r1, [pc]  (literal at +16) */
    wr32(data, so + 12, 0xea000000u | ((uint32_t)rel & 0xffffffu)); /* b common */
    wr32(data, so + 16, targets[i]);
    wr32(data, META_TABLE - VADDR_BASE + i * 4, sv);
    so += META_STUB_BYTES;
    sv += META_STUB_BYTES;
  }
  return true;
}
