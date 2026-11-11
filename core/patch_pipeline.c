/* patch_pipeline.c -- see patch_pipeline.h. */
#include "patch_pipeline.h"

#include "elf_patch.h"
#include "ext4_image.h"
#include "fit_image.h"
#include "launcher_script.h"
#include "spy_patch.h"
#include "model_targets.h"
#include "nam_blobs_embedded.h"
#include "page_pedal.h"
#include "qml_patch.h"
#include "xz_codec.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

static void report(NamProgressFn progress, void* user_data, const char* fmt, ...)
{
  if (!progress)
    return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  progress(buf, user_data);
}

static uint8_t* read_whole_file_or_null(const char* path, size_t* out_len, char* err, size_t err_size)
{
  FILE* f = fopen(path, "rb");
  if (!f)
  {
    set_err(err, err_size, "opening %s: %s", path, strerror(errno));
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0)
  {
    set_err(err, err_size, "ftell %s failed", path);
    fclose(f);
    return NULL;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz ? (size_t)sz : 1);
  if (!buf)
  {
    set_err(err, err_size, "out of memory reading %s (%ld bytes)", path, sz);
    fclose(f);
    return NULL;
  }
  if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz)
  {
    set_err(err, err_size, "short read on %s", path);
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_len = (size_t)sz;
  return buf;
}

static bool write_whole_file(const char* path, const uint8_t* data, size_t len, char* err, size_t err_size)
{
  FILE* f = fopen(path, "wb");
  if (!f)
  {
    set_err(err, err_size, "opening %s for write: %s", path, strerror(errno));
    return false;
  }
  bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
  fclose(f);
  if (!ok)
    set_err(err, err_size, "short write on %s", path);
  return ok;
}

bool nam_patch_pipeline(const uint8_t* stock_img_data, size_t stock_img_len, const char* model_name,
                         bool enable_v2_hijack, bool enable_page_pedal, bool additive_nam, const char* workdir,
                         NamProgressFn progress, void* progress_user_data, uint8_t** out_img_data, size_t* out_img_len,
                         char* err, size_t err_size)
{
  *out_img_data = NULL;
  *out_img_len = 0;

  FitProps props = {0};
  FitProps verify_props = {0};
  bool props_valid = false, verify_props_valid = false;
  uint8_t* rootfs_bin = NULL;
  uint8_t* evil_data = NULL;
  uint8_t* script_data_raw = NULL;
  char* script_data = NULL;
  char* new_script = NULL;
  uint8_t* rootfs_patched = NULL;
  uint8_t* rootfs_patched_xz = NULL;
  uint8_t* out_fit = NULL;
  uint8_t* verify_rootfs_bin = NULL;
  uint8_t* verify_evil = NULL;
  Ext4Image* img = NULL;
  Ext4Image* verify_img = NULL;
  bool ok = false;

  if (!nam_fit_parse(stock_img_data, stock_img_len, &props))
  {
    set_err(err, err_size, "doesn't look like a valid FIT image (bad FDT magic?)");
    goto cleanup;
  }
  props_valid = true;

  FitMetadata metadata;
  if (!nam_fit_read_root_metadata(stock_img_data, &props, &metadata))
  {
    set_err(err, err_size, "FIT root node is missing description/compatible/inmusic,devices");
    goto cleanup;
  }
  report(progress, progress_user_data, "OK  extracted metadata: description=%s compatible=%s", metadata.description,
         metadata.compatible);

  const char* reason;
  const ModelTarget* target = nam_select_target(model_name, metadata.compatible, &reason);
  if (!target)
  {
    set_err(err, err_size, "%s", reason);
    goto cleanup;
  }
  if (reason)
    report(progress, progress_user_data, "OK  model target: %s  (%s)", target->name, reason);
  else
    report(progress, progress_user_data, "OK  model target: %s", target->name);

  uint32_t splash_off, splash_len, recov_off, recov_len, rootfs_off, rootfs_len;
  if (!nam_fit_get_bytes(&props, "//images/splash", "data", &splash_off, &splash_len)
      || !nam_fit_get_bytes(&props, "//images/recoverysplash", "data", &recov_off, &recov_len)
      || !nam_fit_get_bytes(&props, "//images/rootfs", "data", &rootfs_off, &rootfs_len))
  {
    set_err(err, err_size, "FIT image missing an expected splash/recoverysplash/rootfs image node");
    goto cleanup;
  }
  const uint8_t* splash_xz = stock_img_data + splash_off;
  const uint8_t* recoverysplash_xz = stock_img_data + recov_off;
  const uint8_t* rootfs_xz = stock_img_data + rootfs_off;
  report(progress, progress_user_data, "OK  extracted splash (%uB), recoverysplash (%uB), rootfs.xz (%uB)",
         splash_len, recov_len, rootfs_len);

  size_t rootfs_bin_len;
  if (!nam_xz_decompress(rootfs_xz, rootfs_len, &rootfs_bin, &rootfs_bin_len, err, err_size))
    goto cleanup;

  char rootfs_path[600];
  snprintf(rootfs_path, sizeof(rootfs_path), "%s/rootfs.bin", workdir);
  if (!write_whole_file(rootfs_path, rootfs_bin, rootfs_bin_len, err, err_size))
    goto cleanup;
  free(rootfs_bin);
  rootfs_bin = NULL;

  if (!nam_ext4_open(rootfs_path, true, &img, err, err_size))
    goto cleanup;

  size_t evil_len;
  if (!nam_ext4_dump(img, "/usr/Evil/Evil", &evil_data, &evil_len, err, err_size))
    goto cleanup;

  size_t script_len;
  if (!nam_ext4_dump(img, "/usr/Evil/Scripts/evil", &script_data_raw, &script_len, err, err_size))
    goto cleanup;
  script_data = (char*)malloc(script_len + 1);
  if (!script_data)
  {
    set_err(err, err_size, "out of memory");
    goto cleanup;
  }
  memcpy(script_data, script_data_raw, script_len);
  script_data[script_len] = '\0';
  free(script_data_raw);
  script_data_raw = NULL;

  const uint8_t* tramp = g_nam_trampoline_gonk_bin;
  size_t tramp_len = (size_t)g_nam_trampoline_gonk_bin_len;
  if (tramp_len != 32)
  {
    set_err(err, err_size, "embedded trampoline_gonk.bin is %zu bytes, expected exactly 32", tramp_len);
    goto cleanup;
  }

  ElfPatchResult elf_result = {0};
  ElfPatchResult elf_result_v2 = {0};
  bool has_v2_hijack = false;
  uint32_t eng_hook_slot = 0, eng_init_slot = 0, eng_tab = 0;
  if (additive_nam)
  {
    if (strcmp(target->name, "pedalboard") != 0)
    {
      set_err(err, err_size, "the additive NAM pedals are Pedalboard-only so far; selected target is %s", target->name);
      goto cleanup;
    }
    /* the registry patch parks tables in the zero slack after the R+W segment, which is the very end of the file */
    uint8_t* grown = (uint8_t*)realloc(evil_data, evil_len + 4096);
    if (!grown)
    {
      set_err(err, err_size, "out of memory growing Evil");
      goto cleanup;
    }
    memset(grown + evil_len, 0, 4096);
    evil_data = grown;
    evil_len += 4096;
    const NamPair pairs[2] = {{"NAM A", "NAM A", NAM_ENGINE_TYPE, 0}, {"NAM B", "NAM B", NAM_ENGINE_TYPE, 0}};
    if (!nam_elf_patch_registry(evil_data, evil_len, pairs, 2, err, err_size)
        || !nam_elf_patch_node_registration(evil_data, evil_len, pairs, 2, &eng_init_slot, &eng_tab, err, err_size)
        || !nam_elf_patch_engine_class(evil_data, evil_len, 4, eng_tab, &eng_hook_slot, err, err_size))
      goto cleanup;
    report(progress, progress_user_data,
           "OK  additive NAM pedals registered: \"NAM A\" and \"NAM B\" (2 instances each), engine hook_slot @ 0x%x",
           eng_hook_slot);
  }
  else
  {
    if (!nam_elf_patch_gonkulator(evil_data, evil_len, tramp, target->engine_vtable_vaddr, target->orig_process_fn,
                                   &elf_result, err, err_size))
      goto cleanup;
    report(progress, progress_user_data, "OK  ELF hijack patched: trampoline @ 0x%x, hook_slot @ 0x%x",
           elf_result.trampoline_vaddr, elf_result.hook_slot_addr);

    if (enable_v2_hijack)
    {
      if (!nam_elf_patch_gonkulator(evil_data, evil_len, tramp, target->v2_engine_vtable_vaddr,
                                     target->v2_orig_process_fn, &elf_result_v2, err, err_size))
        goto cleanup;
      has_v2_hijack = true;
      report(progress, progress_user_data, "OK  ELF hijack patched (Anxiety OD V2): trampoline @ 0x%x, hook_slot @ 0x%x",
             elf_result_v2.trampoline_vaddr, elf_result_v2.hook_slot_addr);
    }

    if (target->qml_rename_count > 0)
    {
      if (!nam_qml_patch(evil_data, evil_len, target, err, err_size))
        goto cleanup;
      report(progress, progress_user_data, "OK  QML knob labels relabeled");
    }
    else
    {
      report(progress, progress_user_data, "    (QML knob relabel skipped for %s -- no per-pedal target)",
             target->name);
    }
  }

  if (enable_page_pedal && strcmp(target->name, "pedalboard") != 0)
  {
    set_err(err, err_size,
            "the footswitch page-turner mod is Pedalboard-only (its footswitch->note mapping is the Pedalboard's "
            "12-switch panel); selected target is %s",
            target->name);
    goto cleanup;
  }

  char hook_slot_hex[32];
  snprintf(hook_slot_hex, sizeof(hook_slot_hex), "0x%x", elf_result.hook_slot_addr);
  char hook_slot_eng_hex[32];
  snprintf(hook_slot_eng_hex, sizeof(hook_slot_eng_hex), "0x%x", eng_hook_slot);
  char eng_init_hex[32], eng_tab_hex[32];
  snprintf(eng_init_hex, sizeof(eng_init_hex), "0x%x", eng_init_slot);
  snprintf(eng_tab_hex, sizeof(eng_tab_hex), "0x%x", eng_tab);
  char hook_slot_v2_hex[32];
  if (has_v2_hijack)
    snprintf(hook_slot_v2_hex, sizeof(hook_slot_v2_hex), "0x%x", elf_result_v2.hook_slot_addr);
  if (!nam_build_launcher_script(script_data, additive_nam ? NULL : hook_slot_hex, has_v2_hijack ? hook_slot_v2_hex : NULL,
                                  additive_nam ? hook_slot_eng_hex : NULL, additive_nam ? eng_init_hex : NULL,
                                  additive_nam ? eng_tab_hex : NULL, &new_script,
                                  err, err_size))
    goto cleanup;
  free(script_data);
  script_data = NULL;

  if (enable_page_pedal)
  {
    char* with_pp = NULL;
    if (!nam_launcher_add_page_pedal(new_script, &with_pp, err, err_size))
      goto cleanup;
    free(new_script);
    new_script = with_pp;
    if (!nam_page_pedal_install(img, g_pagepedal_bin, (size_t)g_pagepedal_bin_len, err, err_size))
      goto cleanup;
    report(progress, progress_user_data,
           "OK  page-turner installed: /usr/Evil/pagepedal, pagepedal-usb.sh, USB mode scripts patched");
  }

  if (!nam_ext4_inject(img, "/usr/Evil/Evil", evil_data, evil_len, 0755, err, err_size))
    goto cleanup;

  const uint8_t* hook_so = g_nam_hook_so;
  size_t hook_so_len = (size_t)g_nam_hook_so_len;
  const uint8_t* preload_so = g_nam_preload_so;
  size_t preload_so_len = (size_t)g_nam_preload_so_len;

  if (!nam_ext4_inject(img, "/usr/Evil/libnam_hook.so", hook_so, hook_so_len, 0755, err, err_size))
    goto cleanup;
  if (!nam_ext4_inject(img, "/usr/Evil/libnam_preload.so", preload_so, preload_so_len, 0755, err, err_size))
    goto cleanup;
  if (!nam_ext4_inject(img, "/usr/Evil/Scripts/evil", (const uint8_t*)new_script, strlen(new_script), 0755, err,
                        err_size))
    goto cleanup;
  report(progress, progress_user_data,
         "OK  injected patched Evil, libnam_hook.so, libnam_preload.so, launcher script");

  if (!nam_ext4_verify_basic(img, err, err_size))
    goto cleanup;
  report(progress, progress_user_data, "OK  ext4 self-consistency check passed");

  nam_ext4_close(img);
  img = NULL;
  free(new_script);
  new_script = NULL;

  size_t rootfs_patched_len;
  rootfs_patched = read_whole_file_or_null(rootfs_path, &rootfs_patched_len, err, err_size);
  if (!rootfs_patched)
    goto cleanup;

  size_t rootfs_patched_xz_len;
  if (!nam_xz_compress(rootfs_patched, rootfs_patched_len, &rootfs_patched_xz, &rootfs_patched_xz_len, err, err_size))
    goto cleanup;

  size_t out_fit_len;
  if (!nam_fit_build(&metadata, splash_xz, splash_len, recoverysplash_xz, recov_len, rootfs_patched_xz,
                      rootfs_patched_xz_len, &out_fit, &out_fit_len))
  {
    set_err(err, err_size, "building output FIT image (out of memory?)");
    goto cleanup;
  }
  report(progress, progress_user_data, "OK  built patched Update.img (%zu bytes)", out_fit_len);

  /* ---- round-trip verification ---- */
  if (!nam_fit_parse(out_fit, out_fit_len, &verify_props))
  {
    set_err(err, err_size, "round-trip FAILED: couldn't re-parse the FIT image we just wrote");
    goto cleanup;
  }
  verify_props_valid = true;

  uint32_t verify_rootfs_off, verify_rootfs_xz_len;
  if (!nam_fit_get_bytes(&verify_props, "//images/rootfs", "data", &verify_rootfs_off, &verify_rootfs_xz_len))
  {
    set_err(err, err_size, "round-trip FAILED: rebuilt FIT is missing the rootfs image node");
    goto cleanup;
  }

  size_t verify_rootfs_bin_len;
  if (!nam_xz_decompress(out_fit + verify_rootfs_off, verify_rootfs_xz_len, &verify_rootfs_bin,
                          &verify_rootfs_bin_len, err, err_size))
    goto cleanup;

  char verify_rootfs_path[600];
  snprintf(verify_rootfs_path, sizeof(verify_rootfs_path), "%s/verify_rootfs.bin", workdir);
  if (!write_whole_file(verify_rootfs_path, verify_rootfs_bin, verify_rootfs_bin_len, err, err_size))
    goto cleanup;
  free(verify_rootfs_bin);
  verify_rootfs_bin = NULL;

  if (!nam_ext4_open(verify_rootfs_path, false, &verify_img, err, err_size))
    goto cleanup;

  size_t verify_evil_len;
  if (!nam_ext4_dump(verify_img, "/usr/Evil/Evil", &verify_evil, &verify_evil_len, err, err_size))
    goto cleanup;

  if (verify_evil_len != evil_len || memcmp(verify_evil, evil_data, evil_len) != 0)
  {
    set_err(err, err_size, "round-trip FAILED: repacked Update.img's Evil binary doesn't match what was built");
    goto cleanup;
  }
  report(progress, progress_user_data, "OK  round-trip verified: repacked image's /usr/Evil/Evil is byte-exact");

  *out_img_data = out_fit;
  *out_img_len = out_fit_len;
  out_fit = NULL; /* ownership transferred to caller */
  ok = true;

cleanup:
  if (props_valid)
    nam_fit_props_free(&props);
  if (verify_props_valid)
    nam_fit_props_free(&verify_props);
  if (img)
    nam_ext4_close(img);
  if (verify_img)
    nam_ext4_close(verify_img);
  free(rootfs_bin);
  free(evil_data);
  free(script_data_raw);
  free(script_data);
  free(new_script);
  free(rootfs_patched);
  free(rootfs_patched_xz);
  free(out_fit);
  free(verify_rootfs_bin);
  free(verify_evil);
  return ok;
}
