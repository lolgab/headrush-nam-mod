/* mods.c -- see mods.h. */
#include "mods.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Model lists are what has been verified, not what might work. The NAM
 * pedals and the page-turner were built and tested on the Pedalboard; the
 * MX5 mods (console, remote screen, tuner tempo) on MX5 2.7 hardware only.
 * mods/README.md covers what porting the MX5 mods to the other models
 * needs. */
const NamModInfo NAM_MODS[NAM_MOD_COUNT] = {
  {
    .id = NAM_MOD_NAM_PEDALS,
    .key = "nam-pedals",
    .title = "Add the NAM pedals (\"NAM\", \"NAM TWO\": four per rig)",
    .description = "Adds two new pedals to the Distortion list that play Neural Amp Modeler models. Nothing stock "
                   "is changed.",
    .models = {"pedalboard"},
    .requires = NAM_MOD_COUNT,
  },
  {
    .id = NAM_MOD_ANXIETY,
    .key = "anxiety-hijack",
    .title = "Make Anxiety OD run NAM (up to 2 instances)",
    .description = "Replaces the Anxiety OD pedal's DSP with a Neural Amp Modeler player. On the Pedalboard, this "
                   "keeps pedalboards built with the older version of this mod working.",
    .models = {"pedalboard", "mx5", "gigboard"},
    .requires = NAM_MOD_COUNT,
  },
  {
    .id = NAM_MOD_ANXIETY_V2,
    .key = "anxiety-v2",
    .title = "Also make Anxiety OD V2 run NAM (up to 4 instances)",
    .description = "Hijacks Anxiety OD V2 the same way, for up to 4 hijacked instances per rig.",
    .models = {"pedalboard", "mx5", "gigboard"},
    .requires = NAM_MOD_ANXIETY,
  },
  {
    .id = NAM_MOD_PAGE_PEDAL,
    .key = "page-pedal",
    .title = "Footswitch page-turner (FS5/FS11 -> USB Page Down/Up)",
    .description = "Two footswitches act as a USB keyboard's Page Up/Page Down for the connected computer, e.g. "
                   "to turn sheet-music pages. See docs/PAGE_PEDAL.md.",
    .models = {"pedalboard"},
    .requires = NAM_MOD_COUNT,
  },
  {
    .id = NAM_MOD_USB_CONSOLE,
    .key = "usb-console",
    .title = "USB serial console (root shell over USB)",
    .description = "The device shows up as two USB serial ports when connected to a computer: a root shell, "
                   "and a free data channel.",
    .models = {"mx5"},
    .requires = NAM_MOD_COUNT,
  },
  {
    .id = NAM_MOD_REMOTE_SCREEN,
    .key = "remote-screen",
    .title = "Remote screen (view and control the screen over USB)",
    .description = "Starts a screen server at boot; open mods/remote_screen/mx5_viewer.html in Chrome/Edge to "
                   "see and use the device's screen.",
    .models = {"mx5"},
    .requires = NAM_MOD_USB_CONSOLE,
  },
  {
    .id = NAM_MOD_TUNER_TEMPO,
    .key = "tuner-tempo",
    .title = "Tuner tempo edit (tap tempo box to change BPM)",
    .description = "On the tuner screen, tap the left/right half of the tempo box to step BPM down/up.",
    .models = {"mx5"},
    .requires = NAM_MOD_COUNT,
  },
};

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

NamModSelection nam_mods_default(const char* model_name)
{
  NamModSelection sel;
  memset(&sel, 0, sizeof(sel));
  if (model_name && nam_mod_supports_model(&NAM_MODS[NAM_MOD_NAM_PEDALS], model_name))
    sel.enabled[NAM_MOD_NAM_PEDALS] = true;
  else
    sel.enabled[NAM_MOD_ANXIETY] = true;
  return sel;
}

const NamModInfo* nam_mod_by_key(const char* key)
{
  for (int i = 0; i < NAM_MOD_COUNT; ++i)
    if (strcmp(NAM_MODS[i].key, key) == 0)
      return &NAM_MODS[i];
  return NULL;
}

bool nam_mod_supports_model(const NamModInfo* mod, const char* model_name)
{
  for (int i = 0; i < NAM_MOD_MAX_MODELS && mod->models[i]; ++i)
    if (strcmp(mod->models[i], model_name) == 0)
      return true;
  return false;
}

const char* nam_mod_models_str(const NamModInfo* mod)
{
  static char buf[128];
  buf[0] = '\0';
  for (int i = 0; i < NAM_MOD_MAX_MODELS && mod->models[i]; ++i)
  {
    if (i > 0)
      strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
    strncat(buf, mod->models[i], sizeof(buf) - strlen(buf) - 1);
  }
  return buf;
}

bool nam_mods_validate(const NamModSelection* sel, const char* model_name, char* err, size_t err_size)
{
  bool any = false;
  for (int i = 0; i < NAM_MOD_COUNT; ++i)
  {
    if (!sel->enabled[i])
      continue;
    any = true;
    const NamModInfo* mod = &NAM_MODS[i];
    if (!nam_mod_supports_model(mod, model_name))
    {
      set_err(err, err_size, "mod '%s' is not available for %s (supported: %s)", mod->key, model_name,
              nam_mod_models_str(mod));
      return false;
    }
    if (mod->requires != NAM_MOD_COUNT && !sel->enabled[mod->requires])
    {
      set_err(err, err_size, "mod '%s' requires mod '%s' -- select both", mod->key, NAM_MODS[mod->requires].key);
      return false;
    }
  }
  if (!any)
  {
    set_err(err, err_size, "no mods selected -- nothing to install");
    return false;
  }
  return true;
}

const char* nam_mods_selection_str(const NamModSelection* sel)
{
  static char buf[256];
  buf[0] = '\0';
  for (int i = 0; i < NAM_MOD_COUNT; ++i)
  {
    if (!sel->enabled[i])
      continue;
    if (buf[0])
      strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
    strncat(buf, NAM_MODS[i].key, sizeof(buf) - strlen(buf) - 1);
  }
  return buf;
}
