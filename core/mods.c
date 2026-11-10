/* mods.c -- see mods.h. */
#include "mods.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Model lists are what has been verified, not what might work. The three
 * non-NAM mods were built and confirmed on MX5 2.7 hardware only; see
 * mods/README.md for what porting each one to Pedalboard/Gigboard needs. */
const NamModInfo NAM_MODS[NAM_MOD_COUNT] = {
  {
    .id = NAM_MOD_NAM,
    .key = "nam",
    .title = "NAM models in Anxiety OD (up to 2 instances)",
    .description = "Replaces the Anxiety OD pedal's DSP with a Neural Amp Modeler player.",
    .models = {"pedalboard", "mx5", "gigboard"},
    .requires = NAM_MOD_COUNT,
  },
  {
    .id = NAM_MOD_NAM_4X,
    .key = "nam-4x",
    .title = "Also use Anxiety OD V2 for NAM (up to 4 instances)",
    .description = "Hijacks Anxiety OD V2 the same way, for up to 4 NAM instances per rig.",
    .models = {"pedalboard", "mx5", "gigboard"},
    .requires = NAM_MOD_NAM,
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

NamModSelection nam_mods_default(void)
{
  NamModSelection sel;
  memset(&sel, 0, sizeof(sel));
  sel.enabled[NAM_MOD_NAM] = true;
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
