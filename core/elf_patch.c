/* elf_patch.c -- see elf_patch.h and patch/patch_gonkulator.py. */
#include "elf_patch.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_LOAD 1u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define PROCESS_SLOT 8u
#define VADDR_BASE 0x8000u
#define TRAMP_CODE_LEN 28u
#define PHDR_OFF_FILESZ 16u
#define PHDR_OFF_MEMSZ 20u

typedef struct
{
  uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Phdr;

static uint32_t rd32(const uint8_t* d, size_t off)
{
  return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) | ((uint32_t)d[off + 2] << 16)
         | ((uint32_t)d[off + 3] << 24);
}

static uint16_t rd16(const uint8_t* d, size_t off)
{
  return (uint16_t)(d[off] | (d[off + 1] << 8));
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

bool nam_elf_patch_gonkulator(uint8_t* data, size_t data_len, const uint8_t tramp_in[32], uint32_t engine_vtable,
                               uint32_t orig_fn, ElfPatchResult* result, char* err, size_t err_size)
{
  if (data_len < 52)
  {
    set_err(err, err_size, "file too small to be an ELF binary");
    return false;
  }
  if (memcmp(data, "\x7f"
                    "ELF",
             4)
      != 0)
  {
    set_err(err, err_size, "not an ELF file");
    return false;
  }
  if (data[4] != 1)
  {
    set_err(err, err_size, "not a 32-bit ELF");
    return false;
  }
  if (data[5] != 1)
  {
    set_err(err, err_size, "not a little-endian ELF");
    return false;
  }

  uint16_t e_type = rd16(data, 16);
  uint16_t e_machine = rd16(data, 18);
  uint32_t e_phoff = rd32(data, 28);
  uint16_t e_phentsize = rd16(data, 42);
  uint16_t e_phnum = rd16(data, 44);

  if (e_machine != 40)
  {
    set_err(err, err_size, "not ARM (EM_ARM=40), got e_machine=%u", (unsigned)e_machine);
    return false;
  }
  if (e_type != 2)
  {
    set_err(err, err_size, "expected ET_EXEC (non-PIE), got e_type=%u", (unsigned)e_type);
    return false;
  }
  if ((size_t)e_phoff + (size_t)e_phentsize * e_phnum > data_len)
  {
    set_err(err, err_size, "program header table out of bounds");
    return false;
  }

  Phdr* phdrs = (Phdr*)malloc(sizeof(Phdr) * e_phnum);
  if (!phdrs)
  {
    set_err(err, err_size, "out of memory");
    return false;
  }
  for (uint16_t i = 0; i < e_phnum; ++i)
  {
    size_t off = (size_t)e_phoff + (size_t)i * e_phentsize;
    phdrs[i].p_type = rd32(data, off);
    phdrs[i].p_offset = rd32(data, off + 4);
    phdrs[i].p_vaddr = rd32(data, off + 8);
    phdrs[i].p_paddr = rd32(data, off + 12);
    phdrs[i].p_filesz = rd32(data, off + 16);
    phdrs[i].p_memsz = rd32(data, off + 20);
    phdrs[i].p_flags = rd32(data, off + 24);
    phdrs[i].p_align = rd32(data, off + 28);
  }

  uint32_t slot_vaddr = engine_vtable + PROCESS_SLOT * 4;
  uint32_t slot_file_offset = slot_vaddr - VADDR_BASE;
  if ((size_t)slot_file_offset + 4 > data_len)
  {
    set_err(err, err_size, "vtable slot file offset out of bounds");
    free(phdrs);
    return false;
  }

  uint32_t orig_val = rd32(data, slot_file_offset);
  if (orig_val != orig_fn)
  {
    set_err(err, err_size,
            "REFUSING: AnxietyOD vtable slot @ 0x%x holds 0x%x, expected 0x%x -- wrong binary / wrong --model / "
            "already patched / stale offsets -- re-verify before proceeding.",
            (unsigned)slot_file_offset, (unsigned)orig_val, (unsigned)orig_fn);
    free(phdrs);
    return false;
  }

  int code_idx = -1, data_idx = -1, code_count = 0, data_count = 0;
  for (uint16_t i = 0; i < e_phnum; ++i)
  {
    if (phdrs[i].p_type != PT_LOAD)
      continue;
    if (phdrs[i].p_flags == (PF_R | PF_X))
    {
      code_idx = i;
      code_count++;
    }
    else if (phdrs[i].p_flags == (PF_R | PF_W))
    {
      data_idx = i;
      data_count++;
    }
  }
  if (code_count != 1 || data_count != 1)
  {
    set_err(err, err_size,
            "REFUSING: expected exactly one plain R+E LOAD segment and one plain R+W LOAD segment, found %d and %d "
            "-- binary layout changed, re-verify before proceeding.",
            code_count, data_count);
    free(phdrs);
    return false;
  }

  Phdr* code_seg = &phdrs[code_idx];
  Phdr* data_seg = &phdrs[data_idx];

  uint32_t code_file_end = code_seg->p_offset + code_seg->p_filesz;

  int next_idx = -1;
  for (uint16_t i = 0; i < e_phnum; ++i)
  {
    if (phdrs[i].p_type != PT_LOAD)
      continue;
    if ((int)i == code_idx)
      continue;
    if (phdrs[i].p_offset < code_file_end)
      continue;
    if (next_idx == -1 || phdrs[i].p_offset < phdrs[next_idx].p_offset)
      next_idx = i;
  }
  if (next_idx == -1)
  {
    set_err(err, err_size, "REFUSING: no later PT_LOAD segment found to bound the code cave -- binary layout "
                            "changed, re-verify before proceeding.");
    free(phdrs);
    return false;
  }

  uint32_t gap_size = phdrs[next_idx].p_offset - code_file_end;
  if (gap_size < TRAMP_CODE_LEN)
  {
    set_err(err, err_size,
            "REFUSING: no verified-zero, currently-unmapped-by-any-segment gap of >= %u bytes found right after "
            "the R+E segment's file end -- binary layout changed, re-verify before proceeding.",
            TRAMP_CODE_LEN);
    free(phdrs);
    return false;
  }
  for (uint32_t i = 0; i < gap_size; ++i)
  {
    if (data[code_file_end + i] != 0)
    {
      set_err(err, err_size, "REFUSING: inter-segment gap is not genuinely dead space (non-zero byte at offset "
                              "0x%x) -- binary layout changed, re-verify before proceeding.",
              (unsigned)(code_file_end + i));
      free(phdrs);
      return false;
    }
  }

  uint32_t cave_off = code_file_end;
  uint32_t tramp_base_vaddr = code_seg->p_vaddr + code_seg->p_filesz;
  uint32_t new_code_filesz = code_seg->p_filesz + gap_size;
  uint32_t new_code_memsz = code_seg->p_memsz + gap_size;

  uint32_t hook_slot_addr = data_seg->p_vaddr + data_seg->p_memsz;
  uint32_t new_data_memsz = data_seg->p_memsz + 4;

  uint8_t tramp[32];
  memcpy(tramp, tramp_in, 32);
  wr32(tramp, 20, hook_slot_addr);
  wr32(tramp, 24, orig_fn);

  memcpy(data + cave_off, tramp, TRAMP_CODE_LEN);

  size_t code_phdr_base = (size_t)e_phoff + (size_t)code_idx * e_phentsize;
  wr32(data, code_phdr_base + PHDR_OFF_FILESZ, new_code_filesz);
  wr32(data, code_phdr_base + PHDR_OFF_MEMSZ, new_code_memsz);

  size_t data_phdr_memsz_off = (size_t)e_phoff + (size_t)data_idx * e_phentsize + PHDR_OFF_MEMSZ;
  wr32(data, data_phdr_memsz_off, new_data_memsz);

  wr32(data, slot_file_offset, tramp_base_vaddr);

  free(phdrs);

  result->hook_slot_addr = hook_slot_addr;
  result->trampoline_vaddr = tramp_base_vaddr;
  return true;
}
