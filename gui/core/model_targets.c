#include "model_targets.h"

#include <stdio.h>
#include <string.h>

const ModelTarget NAM_MODEL_TARGETS[NAM_MODEL_COUNT] = {
  /* HeadRush Pedalboard 2.7 -- the original, real-hardware-confirmed target. */
  {
    .name = "pedalboard",
    .match_compatible = "inmusic,mg01",
    .engine_vtable_vaddr = 0x1839044,
    .orig_process_fn = 0x3260e0,
    .qml_rename_count = 3,
    .qml_renames =
      {
        {0x1b7beba, "Drive", "Model"},
        {0x1b7bf1b, "Tone", "Inp "},
        {0x1b7be57, "Level", "Outp "},
      },
  },
  /* HeadRush MX5 2.7 (final release). No per-pedal QML relabel target --
   * knob names come from a shared string pool. */
  {
    .name = "mx5",
    .match_compatible = "inmusic,hg04",
    .engine_vtable_vaddr = 0x17ee460,
    .orig_process_fn = 0x302ed0,
    .qml_rename_count = 0,
  },
  /* HeadRush Gigboard 2.7 -- derived and cross-checked by static analysis
   * only, NOT YET flashed to a real Gigboard. See model_targets.py. */
  {
    .name = "gigboard",
    .match_compatible = "inmusic,hg02",
    .engine_vtable_vaddr = 0x17f2234,
    .orig_process_fn = 0x302840,
    .qml_rename_count = 0,
  },
};

static const ModelTarget* find_by_name(const char* name)
{
  for (int i = 0; i < NAM_MODEL_COUNT; ++i)
    if (strcmp(NAM_MODEL_TARGETS[i].name, name) == 0)
      return &NAM_MODEL_TARGETS[i];
  return NULL;
}

const ModelTarget* nam_select_target(const char* model_name, const char* compatible, const char** reason_out)
{
  static char reason_buf[256];

  if (model_name)
  {
    const ModelTarget* t = find_by_name(model_name);
    if (!t)
    {
      if (reason_out)
        *reason_out = NULL;
      return NULL;
    }
    if (reason_out)
    {
      snprintf(reason_buf, sizeof(reason_buf), "explicit --model %s", model_name);
      *reason_out = reason_buf;
    }
    return t;
  }

  if (compatible)
  {
    for (int i = 0; i < NAM_MODEL_COUNT; ++i)
    {
      if (NAM_MODEL_TARGETS[i].match_compatible && strcmp(NAM_MODEL_TARGETS[i].match_compatible, compatible) == 0)
      {
        if (reason_out)
        {
          snprintf(reason_buf, sizeof(reason_buf), "auto-detected from compatible=%s", compatible);
          *reason_out = reason_buf;
        }
        return &NAM_MODEL_TARGETS[i];
      }
    }
  }

  if (reason_out)
  {
    snprintf(reason_buf, sizeof(reason_buf),
             "defaulted to %s (compatible=%s matched no specific target; pass --model to override)",
             NAM_MODEL_TARGETS[NAM_DEFAULT_TARGET_INDEX].name, compatible ? compatible : "(none)");
    *reason_out = reason_buf;
  }
  return &NAM_MODEL_TARGETS[NAM_DEFAULT_TARGET_INDEX];
}
