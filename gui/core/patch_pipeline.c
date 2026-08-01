/* patch_pipeline.c -- see patch_pipeline.h. */
#include "patch_pipeline.h"

#include "elf_patch.h"
#include "ext4_image.h"
#include "fit_image.h"
#include "launcher_script.h"
#include "model_targets.h"
#include "nam_blobs_embedded.h"
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
                         const char* workdir, NamProgressFn progress, void* progress_user_data,
                         uint8_t** out_img_data, size_t* out_img_len, char* err, size_t err_size)
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
    set_err(err, err_size, "unknown --model %s", model_name);
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

  ElfPatchResult elf_result;
  if (!nam_elf_patch_gonkulator(evil_data, evil_len, tramp, target->engine_vtable_vaddr, target->orig_process_fn,
                                 &elf_result, err, err_size))
    goto cleanup;
  report(progress, progress_user_data, "OK  ELF hijack patched: trampoline @ 0x%x, hook_slot @ 0x%x",
         elf_result.trampoline_vaddr, elf_result.hook_slot_addr);

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

  char hook_slot_hex[32];
  snprintf(hook_slot_hex, sizeof(hook_slot_hex), "0x%x", elf_result.hook_slot_addr);
  if (!nam_build_launcher_script(script_data, hook_slot_hex, &new_script, err, err_size))
    goto cleanup;
  free(script_data);
  script_data = NULL;

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
