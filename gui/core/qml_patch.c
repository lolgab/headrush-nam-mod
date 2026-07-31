/* qml_patch.c -- see qml_patch.h and patch/patch_qml_labels.py. */
#include "qml_patch.h"

#include <stdarg.h>
#include <stdio.h>
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

bool nam_qml_patch(uint8_t* data, size_t data_len, const ModelTarget* target, char* err, size_t err_size)
{
  if (target->qml_rename_count == 0)
  {
    set_err(err, err_size,
            "REFUSING: model %s has no QML knob relabel defined -- do not call nam_qml_patch for it.",
            target->name);
    return false;
  }

  for (int i = 0; i < target->qml_rename_count; ++i)
  {
    const QmlRename* r = &target->qml_renames[i];
    size_t expected_len = strlen(r->expected_text);
    size_t new_len = strlen(r->new_text);
    if (new_len != expected_len)
    {
      set_err(err, err_size, "replacement %s must be exactly as long as %s", r->new_text, r->expected_text);
      return false;
    }
    if ((size_t)r->file_offset + expected_len > data_len)
    {
      set_err(err, err_size, "offset 0x%x + %zu bytes is out of bounds", (unsigned)r->file_offset, expected_len);
      return false;
    }
    if (memcmp(data + r->file_offset, r->expected_text, expected_len) != 0)
    {
      char actual[64];
      size_t n = expected_len < sizeof(actual) - 1 ? expected_len : sizeof(actual) - 1;
      memcpy(actual, data + r->file_offset, n);
      actual[n] = '\0';
      set_err(err, err_size,
              "REFUSING: offset 0x%x holds %s, expected %s -- binary layout changed, re-verify before proceeding.",
              (unsigned)r->file_offset, actual, r->expected_text);
      return false;
    }
    memcpy(data + r->file_offset, r->new_text, new_len);
  }
  return true;
}
