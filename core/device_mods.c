/* device_mods.c -- see device_mods.h. */
#include "device_mods.h"

#include "mod_payloads_embedded.h"

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

#define SCRIPTS_DIR "/usr/Evil/Scripts"
#define UNITS_DIR "/lib/systemd/system"
#define WANTS_DIR "/etc/systemd/system/multi-user.target.wants"

typedef struct
{
  const char* dest;
  const unsigned char* data;
  const unsigned long* len;
  uint32_t mode;
} PayloadFile;

static bool write_payloads(Ext4Image* img, const PayloadFile* files, int count, NamProgressFn progress,
                           void* user_data, char* err, size_t err_size)
{
  for (int i = 0; i < count; ++i)
  {
    if (!nam_ext4_inject(img, files[i].dest, files[i].data, (size_t)*files[i].len, files[i].mode, err, err_size))
      return false;
    report(progress, user_data, "OK  added %s", files[i].dest);
  }
  return true;
}

static bool enable_unit(Ext4Image* img, const char* unit, NamProgressFn progress, void* user_data, char* err,
                        size_t err_size)
{
  char link[256], target[256];
  snprintf(link, sizeof(link), "%s/%s", WANTS_DIR, unit);
  snprintf(target, sizeof(target), "%s/%s", UNITS_DIR, unit);
  if (!nam_ext4_symlink(img, link, target, err, err_size))
    return false;
  report(progress, user_data, "OK  enabled %s", unit);
  return true;
}

/* ---- usb-console ------------------------------------------------------ */

static const char* const TEARDOWN =
  "\n# Release the USB serial console gadget -- this SoC has a single UDC, which\n"
  "# the gadget below needs to claim. [added by headrush-nam-mod: usb-console]\n"
  "[ -x /usr/Evil/Scripts/remove-usb-console.sh ] && /usr/Evil/Scripts/remove-usb-console.sh\n";

/* Inserts TEARDOWN next to the first occurrence of `anchor` in the stock
 * script at `path`. Refuses if the anchor is missing: that means the
 * firmware's USB mode scripts aren't the ones this was verified against. */
static bool patch_mode_script(Ext4Image* img, const char* path, const char* anchor, bool before,
                              NamProgressFn progress, void* user_data, char* err, size_t err_size)
{
  uint8_t* raw = NULL;
  size_t len = 0;
  if (!nam_ext4_dump(img, path, &raw, &len, err, err_size))
    return false;
  char* text = (char*)malloc(len + 1);
  if (!text)
  {
    free(raw);
    set_err(err, err_size, "out of memory");
    return false;
  }
  memcpy(text, raw, len);
  text[len] = '\0';
  free(raw);

  if (strlen(text) != len)
  {
    free(text);
    set_err(err, err_size, "REFUSING: %s contains NUL bytes -- not the shell script expected", path);
    return false;
  }
  if (strstr(text, "remove-usb-console"))
  {
    free(text);
    set_err(err, err_size, "REFUSING: %s already references remove-usb-console -- not a stock image?", path);
    return false;
  }
  char* at = strstr(text, anchor);
  if (!at)
  {
    free(text);
    set_err(err, err_size,
            "REFUSING: anchor '%s' not found in %s -- firmware layout changed, re-verify before patching", anchor,
            path);
    return false;
  }

  size_t anchor_len = strlen(anchor), teardown_len = strlen(TEARDOWN);
  size_t prefix_len = (size_t)(at - text);
  size_t out_len = len + teardown_len + 1; /* +1 for the joining "\n" */
  char* out = (char*)malloc(out_len + 1);
  if (!out)
  {
    free(text);
    set_err(err, err_size, "out of memory");
    return false;
  }
  char* p = out;
  memcpy(p, text, prefix_len);
  p += prefix_len;
  if (before)
  {
    memcpy(p, TEARDOWN, teardown_len);
    p += teardown_len;
    *p++ = '\n';
    memcpy(p, anchor, anchor_len);
    p += anchor_len;
  }
  else
  {
    memcpy(p, anchor, anchor_len);
    p += anchor_len;
    *p++ = '\n';
    memcpy(p, TEARDOWN, teardown_len);
    p += teardown_len;
  }
  size_t rest = len - prefix_len - anchor_len;
  memcpy(p, at + anchor_len, rest);
  p += rest;
  *p = '\0';
  free(text);

  bool ok = nam_ext4_inject(img, path, (const uint8_t*)out, (size_t)(p - out), 0755, err, err_size);
  free(out);
  if (ok)
    report(progress, user_data, "OK  patched %s to release the console gadget first", path);
  return ok;
}

bool nam_mod_apply_usb_console(Ext4Image* img, const char* model_name, NamProgressFn progress, void* user_data,
                               char* err, size_t err_size)
{
  if (strcmp(model_name, "mx5") != 0)
  {
    set_err(err, err_size, "REFUSING: usb-console is only verified on mx5, not %s", model_name);
    return false;
  }
  /* The gadget framework this relies on must actually be present. */
  if (!nam_ext4_exists(img, SCRIPTS_DIR "/usb-otg-audio-start.sh")
      || !nam_ext4_exists(img, SCRIPTS_DIR "/setup-mass-storage.sh") || !nam_ext4_exists(img, WANTS_DIR))
  {
    set_err(err, err_size, "REFUSING: stock USB mode scripts / systemd wants dir missing -- unexpected firmware "
                           "layout");
    return false;
  }

  const PayloadFile files[] = {
    {SCRIPTS_DIR "/setup-usb-console.sh", g_mod_setup_usb_console_sh, &g_mod_setup_usb_console_sh_len, 0755},
    {SCRIPTS_DIR "/remove-usb-console.sh", g_mod_remove_usb_console_sh, &g_mod_remove_usb_console_sh_len, 0755},
    {UNITS_DIR "/usb-console.service", g_mod_usb_console_service, &g_mod_usb_console_service_len, 0644},
    {UNITS_DIR "/usb-console-shell.service", g_mod_usb_console_shell_service, &g_mod_usb_console_shell_service_len,
     0644},
  };
  if (!write_payloads(img, files, (int)(sizeof(files) / sizeof(files[0])), progress, user_data, err, err_size))
    return false;
  if (!enable_unit(img, "usb-console.service", progress, user_data, err, err_size)
      || !enable_unit(img, "usb-console-shell.service", progress, user_data, err, err_size))
    return false;

  if (!patch_mode_script(img, SCRIPTS_DIR "/usb-otg-audio-start.sh", "modprobe configfs", true, progress, user_data,
                         err, err_size))
    return false;
  if (!patch_mode_script(img, SCRIPTS_DIR "/setup-mass-storage.sh", ". /usr/Evil/Scripts/def_vars", false, progress,
                         user_data, err, err_size))
    return false;
  return true;
}

/* ---- remote-screen ---------------------------------------------------- */

bool nam_mod_apply_remote_screen(Ext4Image* img, const char* model_name, NamProgressFn progress, void* user_data,
                                 char* err, size_t err_size)
{
  if (strcmp(model_name, "mx5") != 0)
  {
    set_err(err, err_size, "REFUSING: remote-screen is only verified on mx5, not %s", model_name);
    return false;
  }
  if (!nam_ext4_exists(img, SCRIPTS_DIR "/setup-usb-console.sh"))
  {
    set_err(err, err_size, "REFUSING: remote-screen needs the usb-console mod (it provides /dev/ttyGS1), which "
                           "hasn't been applied to this image");
    return false;
  }
  const unsigned char* bin = g_mod_mx5_screen_server;
  size_t bin_len = (size_t)g_mod_mx5_screen_server_len;
  if (bin_len < 20 || memcmp(bin, "\x7f" "ELF", 4) != 0 || bin[18] != 0x28 || bin[19] != 0x00) /* EM_ARM */
  {
    set_err(err, err_size, "embedded mx5_screen_server is not an ARM ELF -- rebuild blobs/");
    return false;
  }

  const PayloadFile files[] = {
    {"/usr/Evil/mx5_screen_server", g_mod_mx5_screen_server, &g_mod_mx5_screen_server_len, 0755},
    {UNITS_DIR "/mx5-screen.service", g_mod_mx5_screen_service, &g_mod_mx5_screen_service_len, 0644},
  };
  if (!write_payloads(img, files, (int)(sizeof(files) / sizeof(files[0])), progress, user_data, err, err_size))
    return false;
  return enable_unit(img, "mx5-screen.service", progress, user_data, err, err_size);
}

/* ---- tuner-tempo ------------------------------------------------------ */

typedef struct
{
  const char* model;
  /* Tuner.qml's plain-text source in Evil's Qt resource data: file offset
   * and exact length (the replacement must be the same length so no Qt
   * resource-table offsets move). */
  uint32_t src_off;
  uint32_t src_len;
  const char* src_prefix;
  /* Tuner.qml's compiled QV4 unit, found by its UTF-16 literals
   * ("focusTunerRef", "tunerRefFocused"). Its 'qv4cdata' magic gets one
   * byte flipped so Qt rejects it and falls back to the source. */
  uint32_t unit_off;
  const unsigned char* payload;
  const unsigned long* payload_len;
} TunerTempoTarget;

static const TunerTempoTarget TUNER_TEMPO_TARGETS[] = {
  /* MX5 2.7 final (compatible "inmusic,hg04"). Confirmed on hardware. */
  {"mx5", 0x187AFC7, 3569, "import \"../PageController\"", 0x2B08C70, g_mod_tuner_mx5_qml, &g_mod_tuner_mx5_qml_len},
};

bool nam_mod_apply_tuner_tempo(uint8_t* evil_data, size_t evil_len, const char* model_name, NamProgressFn progress,
                               void* user_data, char* err, size_t err_size)
{
  const TunerTempoTarget* t = NULL;
  for (size_t i = 0; i < sizeof(TUNER_TEMPO_TARGETS) / sizeof(TUNER_TEMPO_TARGETS[0]); ++i)
    if (strcmp(TUNER_TEMPO_TARGETS[i].model, model_name) == 0)
      t = &TUNER_TEMPO_TARGETS[i];
  if (!t)
  {
    set_err(err, err_size, "REFUSING: tuner-tempo has no verified offsets for %s", model_name);
    return false;
  }

  if ((size_t)*t->payload_len != t->src_len)
  {
    set_err(err, err_size, "embedded Tuner.qml for %s is %lu bytes, must be exactly %u (same-length edit)", t->model,
            *t->payload_len, t->src_len);
    return false;
  }
  /* +1: the source blob is NUL-terminated, checked below. */
  if ((size_t)t->src_off + t->src_len + 1 > evil_len || (size_t)t->unit_off + 8 > evil_len)
  {
    set_err(err, err_size, "REFUSING: tuner-tempo offsets are past the end of Evil -- wrong firmware build");
    return false;
  }

  size_t prefix_len = strlen(t->src_prefix);
  if (memcmp(evil_data + t->src_off, t->src_prefix, prefix_len) != 0 || evil_data[t->src_off + t->src_len] != 0
      || memchr(evil_data + t->src_off, 0, t->src_len) != NULL)
  {
    set_err(err, err_size,
            "REFUSING: Tuner.qml source not found at 0x%x (expected %u bytes starting with %s) -- wrong firmware "
            "build or already patched",
            t->src_off, t->src_len, t->src_prefix);
    return false;
  }
  if (memcmp(evil_data + t->unit_off, "qv4cdata", 8) != 0)
  {
    set_err(err, err_size, "REFUSING: no compiled QML unit at 0x%x -- wrong firmware build or already patched",
            t->unit_off);
    return false;
  }

  memcpy(evil_data + t->src_off, t->payload, t->src_len);
  report(progress, user_data, "OK  replaced Tuner.qml source @0x%x (%u bytes, same length)", t->src_off, t->src_len);
  evil_data[t->unit_off] = 'X';
  report(progress, user_data, "OK  invalidated Tuner compiled unit @0x%x (qv4cdata -> Xv4cdata)", t->unit_off);
  return true;
}
