/* qml_patch.h -- C port of patch/patch_qml_labels.py: same-length ASCII
 * text replace at fixed file offsets (Anxiety OD's on-screen knob labels).
 * See patch_qml_labels.py's own docstring for why these exact offsets are
 * safe to same-length-replace (raw QML source text embedded in .rodata).
 */
#ifndef NAM_GUI_QML_PATCH_H
#define NAM_GUI_QML_PATCH_H

#include "model_targets.h"

#include <stdbool.h>
#include <stddef.h>

/* Applies target->qml_renames to `data` (data_len bytes, modified IN
 * PLACE). Returns false (with a reason in err) if target has no renames
 * (qml_rename_count == 0 -- caller should not call this at all in that
 * case, same as patch_qml_labels.py refusing when qml_renames is None), if
 * data_len is too small for a given offset, or if the expected text isn't
 * found at that offset (refuse-on-mismatch, same as the Python original). */
bool nam_qml_patch(uint8_t* data, size_t data_len, const ModelTarget* target, char* err, size_t err_size);

#endif
