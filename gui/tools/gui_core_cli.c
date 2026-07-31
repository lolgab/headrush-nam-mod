/* gui_core_cli.c -- milestone-1 CLI: same argv shape as
 * scripts/build_update_img.py, built entirely on gui/core (no debugfs/
 * e2fsck/xz/mkimage/python3 subprocess calls). Thin wrapper around
 * core/patch_pipeline.c -- the GUI app calls the exact same pipeline
 * function, see gui/app. POSIX-only for now (mkdtemp) -- Windows support
 * is a later packaging milestone, see
 * .claude/plans/crystalline-baking-moonbeam.md.
 *
 * usage: gui-core-cli <stock.img> <out.img> [--model pedalboard|mx5|gigboard]
 *                      [--blobs-dir DIR] [--keep-work-dir]
 */
#include "patch_pipeline.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void die(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "ERROR: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

static uint8_t* read_whole_file(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    die("opening %s: %s", path, strerror(errno));
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz < 0)
    die("ftell %s: %s", path, strerror(errno));
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)sz ? (size_t)sz : 1);
  if (!buf)
    die("out of memory reading %s (%ld bytes)", path, sz);
  if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    die("short read on %s", path);
  fclose(f);
  *out_len = (size_t)sz;
  return buf;
}

static void write_whole_file(const char* path, const uint8_t* data, size_t len)
{
  FILE* f = fopen(path, "wb");
  if (!f)
    die("opening %s for write: %s", path, strerror(errno));
  if (len > 0 && fwrite(data, 1, len, f) != len)
    die("short write on %s", path);
  fclose(f);
}

static void print_progress(const char* message, void* user_data)
{
  (void)user_data;
  printf("%s\n", message);
}

int main(int argc, char** argv)
{
  if (argc < 3)
    die("usage: %s <stock.img> <out.img> [--model pedalboard|mx5|gigboard] [--blobs-dir DIR] [--keep-work-dir]",
        argv[0]);

  const char* input_img = argv[1];
  const char* output_img = argv[2];
  const char* model_name = NULL;
  const char* blobs_dir = "gui/blobs";
  bool keep_work_dir = false;

  for (int i = 3; i < argc; ++i)
  {
    if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
      model_name = argv[++i];
    else if (strcmp(argv[i], "--blobs-dir") == 0 && i + 1 < argc)
      blobs_dir = argv[++i];
    else if (strcmp(argv[i], "--keep-work-dir") == 0)
      keep_work_dir = true;
    else
      die("unknown argument: %s", argv[i]);
  }

  char workdir[] = "/tmp/headrush-nam-build-XXXXXX";
  if (!mkdtemp(workdir))
    die("creating work directory: %s", strerror(errno));
  printf("working directory: %s\n", workdir);

  size_t input_len;
  uint8_t* input_data = read_whole_file(input_img, &input_len);

  uint8_t* out_data;
  size_t out_len;
  char err[1024];
  bool ok = nam_patch_pipeline(input_data, input_len, model_name, blobs_dir, workdir, print_progress, NULL, &out_data,
                                &out_len, err, sizeof(err));
  free(input_data);

  if (!ok)
  {
    if (!keep_work_dir)
    {
      char cmd[700];
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", workdir);
      system(cmd);
    }
    die("%s", err);
  }

  write_whole_file(output_img, out_data, out_len);
  printf("OK  wrote %s (%zu bytes)\n", output_img, out_len);
  free(out_data);

  if (keep_work_dir)
    printf("--keep-work-dir: build directory left at %s\n", workdir);
  else
  {
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", workdir);
    system(cmd);
  }

  printf("\nOK  Anxiety OD (v1) process() NAM hijack applied.\n");
  return 0;
}
