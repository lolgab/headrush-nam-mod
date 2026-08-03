/* model_targets.h -- C port of patch/model_targets.py.
 *
 * Only the values that vary per firmware live here: the two absolute
 * addresses of the Anxiety OD (v1) hijack (engine vtable + its real
 * process() function), the Update.img `compatible` string for auto-detect,
 * and the QML knob-relabel offsets. See patch/model_targets.py for the
 * full reverse-engineering rationale behind every constant below -- this
 * file intentionally carries none of that narrative, just the data, to stay
 * a trivial mechanical port that's easy to diff against the Python source.
 */
#ifndef NAM_GUI_MODEL_TARGETS_H
#define NAM_GUI_MODEL_TARGETS_H

#include <stdint.h>

#define NAM_MAX_QML_RENAMES 4

typedef struct
{
  uint32_t file_offset;
  const char* expected_text;
  const char* new_text;
} QmlRename;

typedef struct
{
  const char* name;             /* "pedalboard" | "mx5" | "gigboard" */
  const char* match_compatible; /* Update.img root `compatible` string; NULL = never auto-matches */

  uint32_t engine_vtable_vaddr;
  uint32_t orig_process_fn;

  int qml_rename_count; /* 0 = no QML relabel for this model */
  QmlRename qml_renames[NAM_MAX_QML_RENAMES];
} ModelTarget;

#define NAM_MODEL_COUNT 3
extern const ModelTarget NAM_MODEL_TARGETS[NAM_MODEL_COUNT];

/* model_name may be NULL (falls through to compatible-string auto-detect).
 * Returns NULL if model_name is non-NULL and unrecognized, OR if model_name
 * is NULL and compatible matched no specific target -- there is no default
 * fallback, callers must pass an explicit --model in that case. reason_out
 * (if non-NULL) always receives a static string: on success, how the target
 * was chosen; on failure (NULL return), why, for use in the error message.
 * Mirrors select_target()'s (target, reason) return in model_targets.py. */
const ModelTarget* nam_select_target(const char* model_name, const char* compatible, const char** reason_out);

#endif
