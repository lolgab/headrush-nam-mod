/* launcher_script.c -- see launcher_script.h. */
#include "launcher_script.h"

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

#define NAM_MOD_START "# --- NAM mod ---\n"
#define NAM_MOD_END "# --- end NAM mod ---\n"

static const char* const MOD_DESC =
  "# Anxiety OD (v1) process() hijack, optionally also Anxiety OD V2's --\n"
  "# the only NAM path(s) this build applies. One of each hijacked pedal's\n"
  "# knobs (Drive/Tone/Level) now selects/scans .nam model files.\n"
  "# (The additive, own-pedal-type design in patch_namloader.py is NOT\n"
  "# applied here -- see README.md.)\n";

static const char* const MOD_TAIL =
  "# LD_PRELOAD/NAM_HOOK_SLOT_*_ADDR are scoped to the /usr/Evil/Evil exec\n"
  "# below via `env`, NOT exported here -- exporting them shell-wide would\n"
  "# also preload libnam_preload.so into systemd-inhibit (a separate,\n"
  "# dynamically-linked ELF binary launched right below). Its constructor\n"
  "# writes to a hardcoded absolute vaddr valid only inside Evil's own\n"
  "# non-PIE layout; inside systemd-inhibit's unrelated address space that\n"
  "# write hits unmapped memory and segfaults it before it ever forks Evil\n"
  "# -- an infinite crash loop, stuck on the splash screen forever.\n";

#define OLD_EXEC "systemd-inhibit --what=handle-power-key /usr/Evil/Evil"

typedef struct
{
  char* data;
  size_t len;
  size_t cap;
} StrBuf;

static bool sb_reserve(StrBuf* sb, size_t extra)
{
  if (sb->len + extra + 1 <= sb->cap)
    return true;
  size_t new_cap = sb->cap ? sb->cap * 2 : 256;
  while (new_cap < sb->len + extra + 1)
    new_cap *= 2;
  char* n = (char*)realloc(sb->data, new_cap);
  if (!n)
    return false;
  sb->data = n;
  sb->cap = new_cap;
  return true;
}

static bool sb_append(StrBuf* sb, const char* s, size_t len)
{
  if (!sb_reserve(sb, len))
    return false;
  memcpy(sb->data + sb->len, s, len);
  sb->len += len;
  sb->data[sb->len] = '\0';
  return true;
}

static bool sb_append_str(StrBuf* sb, const char* s)
{
  return sb_append(sb, s, strlen(s));
}

bool nam_build_launcher_script(const char* stock_script_text, const char* hook_slot_addr_hex,
                                const char* hook_slot_v2_addr_hex, const char* hook_slot_eng_addr_hex,
                                const char* eng_init_slot_hex, const char* eng_tab_hex, char** out, char* err,
                                size_t err_size)
{
  *out = NULL;

  StrBuf comment = {0};
  bool ok = true;
  ok = ok && sb_append_str(&comment, NAM_MOD_START);
  ok = ok && sb_append_str(&comment, MOD_DESC);
  ok = ok && sb_append_str(&comment, MOD_TAIL);
  ok = ok && sb_append_str(&comment, NAM_MOD_END);
  if (!ok)
  {
    set_err(err, err_size, "out of memory");
    free(comment.data);
    return false;
  }

  StrBuf script = {0};
  const char* start = strstr(stock_script_text, NAM_MOD_START);
  const char* end = start ? strstr(start, NAM_MOD_END) : NULL;

  if (start && end)
  {
    size_t block_end = (size_t)(end - stock_script_text) + strlen(NAM_MOD_END);
    ok = ok && sb_append(&script, stock_script_text, (size_t)(start - stock_script_text));
    ok = ok && sb_append(&script, comment.data, comment.len);
    ok = ok && sb_append_str(&script, stock_script_text + block_end);
  }
  else
  {
    const char* marker = strstr(stock_script_text, "while [ 1 ]");
    if (!marker)
    {
      set_err(err, err_size, "stock launcher script has neither an existing NAM-mod block nor a 'while [ 1 ]' "
                              "marker to insert one before -- stock script layout changed, re-verify before "
                              "proceeding.");
      free(comment.data);
      free(script.data);
      return false;
    }
    ok = ok && sb_append(&script, stock_script_text, (size_t)(marker - stock_script_text));
    ok = ok && sb_append(&script, comment.data, comment.len);
    ok = ok && sb_append_str(&script, "\n");
    ok = ok && sb_append_str(&script, marker);
  }
  free(comment.data);
  if (!ok)
  {
    set_err(err, err_size, "out of memory");
    free(script.data);
    return false;
  }

  /* count occurrences of old_exec */
  int count = 0;
  const char* p = script.data;
  while ((p = strstr(p, OLD_EXEC)) != NULL)
  {
    count++;
    p += strlen(OLD_EXEC);
  }
  if (count != 1)
  {
    set_err(err, err_size,
            "expected exactly 1 occurrence of %s in the launcher script, found %d -- stock script layout changed, "
            "re-verify before proceeding.",
            OLD_EXEC, count);
    free(script.data);
    return false;
  }

  StrBuf final = {0};
  const char* hit = strstr(script.data, OLD_EXEC);
  ok = ok && sb_append(&final, script.data, (size_t)(hit - script.data));
  ok = ok && sb_append_str(&final, "systemd-inhibit --what=handle-power-key env LD_PRELOAD=/usr/Evil/libnam_preload.so");
  if (hook_slot_addr_hex)
  {
    ok = ok && sb_append_str(&final, " NAM_HOOK_SLOT_GONK_ADDR=");
    ok = ok && sb_append_str(&final, hook_slot_addr_hex);
  }
  if (hook_slot_eng_addr_hex)
  {
    ok = ok && sb_append_str(&final, " NAM_HOOK_SLOT_ENG_ADDR=");
    ok = ok && sb_append_str(&final, hook_slot_eng_addr_hex);
  }
  if (eng_init_slot_hex)
  {
    ok = ok && sb_append_str(&final, " NAM_HOOK_SLOT_ENG_INIT_ADDR=");
    ok = ok && sb_append_str(&final, eng_init_slot_hex);
  }
  if (eng_tab_hex)
  {
    ok = ok && sb_append_str(&final, " NAM_ENG_TAB_ADDR=");
    ok = ok && sb_append_str(&final, eng_tab_hex);
  }
  if (hook_slot_v2_addr_hex)
  {
    ok = ok && sb_append_str(&final, " NAM_HOOK_SLOT_GONK_V2_ADDR=");
    ok = ok && sb_append_str(&final, hook_slot_v2_addr_hex);
  }
  ok = ok && sb_append_str(&final, " /usr/Evil/Evil");
  ok = ok && sb_append_str(&final, hit + strlen(OLD_EXEC));
  free(script.data);
  if (!ok)
  {
    set_err(err, err_size, "out of memory");
    free(final.data);
    return false;
  }

  *out = final.data;
  return true;
}

#define PAGE_PEDAL_START "# --- page pedal ---\n"
#define PAGE_PEDAL_END "# --- end page pedal ---\n"

static const char* const PAGE_PEDAL_BODY =
  "# Footswitch page-turner: USB keyboard gadget + footswitch->Page Up/Down\n"
  "# daemon (see docs/PAGE_PEDAL.md). Backgrounded and exit-0 so it can never\n"
  "# delay or break Evil's own startup.\n"
  "[ -x /usr/Evil/Scripts/pagepedal-usb.sh ] && /usr/Evil/Scripts/pagepedal-usb.sh boot >/dev/null 2>&1 &\n";

bool nam_launcher_add_page_pedal(const char* script_text, char** out, char* err, size_t err_size)
{
  *out = NULL;
  StrBuf block = {0};
  bool ok = sb_append_str(&block, PAGE_PEDAL_START) && sb_append_str(&block, PAGE_PEDAL_BODY)
            && sb_append_str(&block, PAGE_PEDAL_END);

  StrBuf script = {0};
  const char* start = strstr(script_text, PAGE_PEDAL_START);
  const char* end = start ? strstr(start, PAGE_PEDAL_END) : NULL;
  if (ok && start && end)
  {
    size_t block_end = (size_t)(end - script_text) + strlen(PAGE_PEDAL_END);
    ok = sb_append(&script, script_text, (size_t)(start - script_text)) && sb_append(&script, block.data, block.len)
         && sb_append_str(&script, script_text + block_end);
  }
  else if (ok)
  {
    const char* marker = strstr(script_text, "while [ 1 ]");
    if (!marker)
    {
      set_err(err, err_size, "launcher script has no 'while [ 1 ]' marker to insert the page-pedal block before -- "
                              "stock script layout changed, re-verify before proceeding.");
      free(block.data);
      free(script.data);
      return false;
    }
    ok = sb_append(&script, script_text, (size_t)(marker - script_text)) && sb_append(&script, block.data, block.len)
         && sb_append_str(&script, "\n") && sb_append_str(&script, marker);
  }
  free(block.data);
  if (!ok)
  {
    set_err(err, err_size, "out of memory");
    free(script.data);
    return false;
  }
  *out = script.data;
  return true;
}
