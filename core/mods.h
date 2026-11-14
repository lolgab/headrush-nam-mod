/* mods.h -- the set of firmware mods this installer can apply, and which
 * of them the user picked.
 *
 * Every mod lives in this repo and is compiled into the installer; the
 * user just ticks the ones they want. There is deliberately no plugin
 * format: adding a mod means adding an entry to NAM_MODS (mods.c) and an
 * apply step in patch_pipeline.c.
 *
 * Mods are independent unless one lists another in `requires`. Each mod
 * declares the models it has been verified on; the pipeline refuses to
 * apply a mod to any other model (offsets and device behavior are
 * model-specific, and a wrong guess can leave a device stuck on the boot
 * splash).
 */
#ifndef NAM_GUI_MODS_H
#define NAM_GUI_MODS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum
{
  NAM_MOD_NAM_PEDALS = 0, /* new "NAM" and "NAM TWO" pedals (Pedalboard), see spy_patch.h */
  NAM_MOD_ANXIETY,        /* NAM in Anxiety OD (v1) -- the original hijack */
  NAM_MOD_ANXIETY_V2,     /* also hijack Anxiety OD V2: up to 4 hijacked instances */
  NAM_MOD_PAGE_PEDAL,     /* footswitches -> USB keyboard Page Down/Up (Pedalboard), see page_pedal.h */
  NAM_MOD_USB_CONSOLE,    /* root shell + data channel over USB serial */
  NAM_MOD_REMOTE_SCREEN, /* mirror/control the screen from a computer */
  NAM_MOD_TUNER_TEMPO,   /* tap the tuner's tempo box to change BPM */
  NAM_MOD_COUNT
} NamModId;

#define NAM_MOD_MAX_MODELS 3

typedef struct
{
  NamModId id;
  const char* key;         /* CLI name, e.g. "usb-console" */
  const char* title;       /* one-line label for the GUI checkbox */
  const char* description; /* one or two sentences */
  /* Model names (see model_targets.h) this mod can be applied to. NULL
   * entries terminate the list early. */
  const char* models[NAM_MOD_MAX_MODELS];
  /* Mod that must also be selected, or NAM_MOD_COUNT for none. */
  NamModId requires;
} NamModInfo;

extern const NamModInfo NAM_MODS[NAM_MOD_COUNT];

typedef struct
{
  bool enabled[NAM_MOD_COUNT];
} NamModSelection;

/* What gets installed when the user picks nothing else: the NAM pedals on
 * the Pedalboard, the Anxiety OD hijack on models without NAM pedals. */
NamModSelection nam_mods_default(const char* model_name);

const NamModInfo* nam_mod_by_key(const char* key);

bool nam_mod_supports_model(const NamModInfo* mod, const char* model_name);

/* Human-readable list of the models a mod supports, e.g. "mx5" or
 * "pedalboard, mx5, gigboard". Returns a static buffer. */
const char* nam_mod_models_str(const NamModInfo* mod);

/* Checks a selection against a model: at least one mod selected, every
 * selected mod supports the model, and every selected mod's requirement
 * is also selected. On failure writes the reason into err. */
bool nam_mods_validate(const NamModSelection* sel, const char* model_name, char* err, size_t err_size);

/* Comma-separated keys of the selected mods, e.g. "anxiety-hijack,usb-console".
 * Returns a static buffer. */
const char* nam_mods_selection_str(const NamModSelection* sel);

#endif
