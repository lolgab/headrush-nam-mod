/* page_pedal.c -- see page_pedal.h. */
#include "page_pedal.h"

#include "nam_blobs_embedded.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPTS_DIR "/usr/Evil/Scripts/"
#define HELPER "/usr/Evil/Scripts/pagepedal-usb.sh"
#define MARKER "pagepedal-usb.sh"

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

static int count_occurrences(const char* hay, const char* needle)
{
  int n = 0;
  for (const char* p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
    ++n;
  return n;
}

/* Returns a malloc'd copy of `text` with `insert` placed before (before=true)
 * or after `anchor`, or with `anchor` replaced by `insert` (replace=true).
 * The anchor must occur exactly once. */
typedef enum
{
  EDIT_BEFORE,
  EDIT_AFTER,
  EDIT_REPLACE
} EditMode;

static char* edit_once(const char* text, const char* anchor, const char* insert, EditMode mode, const char* what,
                       char* err, size_t err_size)
{
  int n = count_occurrences(text, anchor);
  if (n != 1)
  {
    set_err(err, err_size,
            "%s: expected exactly 1 occurrence of anchor \"%s\", found %d -- stock script layout changed, "
            "re-verify before proceeding.",
            what, anchor, n);
    return NULL;
  }
  const char* hit = strstr(text, anchor);
  size_t head = (size_t)(hit - text);
  size_t alen = strlen(anchor);
  size_t ilen = strlen(insert);
  size_t total = strlen(text) + ilen + 1;
  char* out = (char*)malloc(total);
  if (!out)
  {
    set_err(err, err_size, "out of memory");
    return NULL;
  }
  char* w = out;
  switch (mode)
  {
    case EDIT_BEFORE:
      memcpy(w, text, head), w += head;
      memcpy(w, insert, ilen), w += ilen;
      memcpy(w, hit, strlen(hit)), w += strlen(hit);
      break;
    case EDIT_AFTER:
      memcpy(w, text, head + alen), w += head + alen;
      memcpy(w, insert, ilen), w += ilen;
      memcpy(w, hit + alen, strlen(hit + alen)), w += strlen(hit + alen);
      break;
    case EDIT_REPLACE:
      memcpy(w, text, head), w += head;
      memcpy(w, insert, ilen), w += ilen;
      memcpy(w, hit + alen, strlen(hit + alen)), w += strlen(hit + alen);
      break;
  }
  *w = '\0';
  return out;
}

static char* append_line(const char* text, const char* line)
{
  size_t tl = strlen(text);
  size_t ll = strlen(line);
  char* out = (char*)malloc(tl + ll + 2);
  if (!out)
    return NULL;
  memcpy(out, text, tl);
  size_t w = tl;
  if (tl > 0 && text[tl - 1] != '\n')
    out[w++] = '\n';
  memcpy(out + w, line, ll);
  w += ll;
  out[w] = '\0';
  return out;
}

#define RELEASE_CALL "[ -x " HELPER " ] && " HELPER " release\n"
#define IDLE_CALL "[ -x " HELPER " ] && " HELPER " idle\n"

bool nam_page_pedal_patch_script(const char* basename, const char* script_text, char** out, char* err,
                                  size_t err_size)
{
  *out = NULL;
  if (strstr(script_text, MARKER))
  {
    *out = strdup(script_text); /* already patched */
    if (!*out)
    {
      set_err(err, err_size, "out of memory");
      return false;
    }
    return true;
  }

  char* a = NULL;
  char* b = NULL;
  char* c = NULL;
  if (strcmp(basename, "usb-otg-audio-start.sh") == 0)
  {
    /* free the UDC from our idle gadget first */
    a = edit_once(script_text, "modprobe configfs",
                  "# Release the footswitch-keyboard gadget (single UDC) [page pedal mod]\n" RELEASE_CALL "\n",
                  EDIT_BEFORE, basename, err, err_size);
    if (!a)
      return false;
    /* give audio mode a keyboard too -- before the UDC bind */
    b = edit_once(a, "    ln -s functions/uac2_az01.otg0 configs/c.1 &&",
                  "\n    { " HELPER " addhid \"$PWD\"; true; } &&", EDIT_AFTER, basename, err, err_size);
    free(a);
    if (!b)
      return false;
    /* ...but if the bind fails with it (endpoint/FIFO budget), retry without,
     * so audio mode can never be broken by this mod */
    c = edit_once(b, "echo ff580000.usb >UDC",
                  "echo ff580000.usb >UDC || { " HELPER " rmhid \"$PWD\"; echo ff580000.usb >UDC; }", EDIT_REPLACE,
                  basename, err, err_size);
    free(b);
    if (!c)
      return false;
    *out = c;
    return true;
  }

  if (strcmp(basename, "usb-otg-audio-stop.sh") == 0)
  {
    a = edit_once(script_text, "echo \"\" >UDC",
                  "echo \"\" >UDC\n    " HELPER " rmhid /sys/kernel/config/usb_gadget/g1", EDIT_REPLACE, basename,
                  err, err_size);
    if (!a)
      return false;
    b = append_line(a, "\n# Bring the footswitch-keyboard gadget back [page pedal mod]\n" IDLE_CALL);
    free(a);
    if (!b)
    {
      set_err(err, err_size, "out of memory");
      return false;
    }
    *out = b;
    return true;
  }

  if (strcmp(basename, "setup-mass-storage.sh") == 0)
  {
    a = edit_once(script_text, ". /usr/Evil/Scripts/def_vars",
                  "\n# Release the footswitch-keyboard gadget (single UDC) [page pedal mod]\n" RELEASE_CALL,
                  EDIT_AFTER, basename, err, err_size);
    if (!a)
      return false;
    *out = a;
    return true;
  }

  if (strcmp(basename, "remove-mass-storage.sh") == 0)
  {
    a = append_line(script_text, "\n# Bring the footswitch-keyboard gadget back [page pedal mod]\n" IDLE_CALL);
    if (!a)
    {
      set_err(err, err_size, "out of memory");
      return false;
    }
    *out = a;
    return true;
  }

  set_err(err, err_size, "page pedal: don't know how to patch %s", basename);
  return false;
}

static bool patch_one(Ext4Image* img, const char* basename, char* err, size_t err_size)
{
  char path[128];
  snprintf(path, sizeof(path), SCRIPTS_DIR "%s", basename);

  uint8_t* raw = NULL;
  size_t raw_len = 0;
  if (!nam_ext4_dump(img, path, &raw, &raw_len, err, err_size))
    return false;
  char* text = (char*)malloc(raw_len + 1);
  if (!text)
  {
    free(raw);
    set_err(err, err_size, "out of memory");
    return false;
  }
  memcpy(text, raw, raw_len);
  text[raw_len] = '\0';
  free(raw);

  char* patched = NULL;
  bool ok = nam_page_pedal_patch_script(basename, text, &patched, err, err_size);
  free(text);
  if (!ok)
    return false;
  ok = nam_ext4_inject(img, path, (const uint8_t*)patched, strlen(patched), 0755, err, err_size);
  free(patched);
  return ok;
}

bool nam_page_pedal_install(Ext4Image* img, const uint8_t* daemon, size_t daemon_len, char* err, size_t err_size)
{
  static const char* const scripts[] = {"usb-otg-audio-start.sh", "usb-otg-audio-stop.sh", "setup-mass-storage.sh",
                                        "remove-mass-storage.sh"};

  /* patch everything first so a layout mismatch fails before we inject anything */
  for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); ++i)
  {
    char path[128];
    uint8_t* raw = NULL;
    size_t raw_len = 0;
    snprintf(path, sizeof(path), SCRIPTS_DIR "%s", scripts[i]);
    if (!nam_ext4_dump(img, path, &raw, &raw_len, err, err_size))
      return false;
    char* text = (char*)malloc(raw_len + 1);
    if (!text)
    {
      free(raw);
      set_err(err, err_size, "out of memory");
      return false;
    }
    memcpy(text, raw, raw_len);
    text[raw_len] = '\0';
    free(raw);
    char* patched = NULL;
    bool ok = nam_page_pedal_patch_script(scripts[i], text, &patched, err, err_size);
    free(text);
    free(patched);
    if (!ok)
      return false;
  }

  if (!nam_ext4_inject(img, "/usr/Evil/pagepedal", daemon, daemon_len, 0755, err, err_size))
    return false;
  if (!nam_ext4_inject(img, HELPER, g_pagepedal_usb_sh, (size_t)g_pagepedal_usb_sh_len, 0755, err, err_size))
    return false;
  for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); ++i)
    if (!patch_one(img, scripts[i], err, err_size))
      return false;
  return true;
}
