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

#include <stdbool.h>
#include <stddef.h>
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

  /* Anxiety OD V2 (internal class name "AnxietyV2") hijack, for the optional
   * "up to 4 instances" mode -- see patch/model_targets.py for the
   * reverse-engineering rationale. Same PROCESS_SLOT(8)/VADDR_BASE(0x8000)
   * as v1; on every device checked so far, V2's process() turned out to be
   * the literal same compiled function as v1's, so orig_process_fn and
   * v2_orig_process_fn are equal by observation, not by assumption -- each
   * hijack still patches its own, independently-guarded vtable slot. */
  uint32_t v2_engine_vtable_vaddr;
  uint32_t v2_orig_process_fn;

  int qml_rename_count; /* 0 = no QML relabel for this model */
  QmlRename qml_renames[NAM_MAX_QML_RENAMES];

  /* SHA-256 (lowercase hex) of the stock Update.img this target was built and tested against: the one inside the
   * official 2.7 updater, macOS and Windows alike. The GUI patches nothing else. */
  const char* stock_img_sha256;
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

/* True if `data` is the stock Update.img `target` was tested against; out_hex (if non-NULL) gets its SHA-256. */
bool nam_is_known_stock_img(const ModelTarget* target, const uint8_t* data, size_t len, char out_hex[65]);

#endif
