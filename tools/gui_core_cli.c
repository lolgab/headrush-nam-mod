/* gui_core_cli.c -- milestone-1 CLI: built entirely on core/ (no debugfs/
 * e2fsck/xz/mkimage/python3 subprocess calls). Thin wrapper around
 * core/patch_pipeline.c -- the GUI app calls the exact same pipeline
 * function, see app/. Work-directory handling is portable (see
 * core/tempdir.h). Local dev/debugging tool only -- not built or shipped
 * by CI, see docs/BUILDING.md.
 *
 * usage: gui-core-cli <stock.img> <out.img> --model pedalboard|mx5|gigboard
 *                      [--anxiety-hijack [--instances 2|4]] [--no-nam-pedals] [--page-pedal]
 *                      [--keep-work-dir]
 *
 * Pedalboard default: adds the "NAM" / "NAM 2" pedals. --anxiety-hijack additionally (opt-in) makes Anxiety OD run
 * NAM (--instances 4 also hijacks Anxiety OD V2); --no-nam-pedals leaves out the new pedals (hijack only). MX5 and
 * Gigboard have no NAM pedals yet and always get the Anxiety OD hijack.
 */
#include "patch_pipeline.h"
#include "tempdir.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  const char* usage =
      "usage: %s <stock.img> <out.img> --model pedalboard|mx5|gigboard [--anxiety-hijack [--instances 2|4]] "
      "[--no-nam-pedals] [--page-pedal] [--keep-work-dir]";
  if (argc < 3)
    die(usage, argv[0]);

  const char* input_img = argv[1];
  const char* output_img = argv[2];
  const char* model_name = NULL;
  bool keep_work_dir = false;
  int instances = 2;
  bool page_pedal = false;
  bool anxiety_hijack = false;
  bool no_nam_pedals = false;

  for (int i = 3; i < argc; ++i)
  {
    if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
      model_name = argv[++i];
    else if (strcmp(argv[i], "--instances") == 0 && i + 1 < argc)
    {
      const char* v = argv[++i];
      if (strcmp(v, "2") == 0)
        instances = 2;
      else if (strcmp(v, "4") == 0)
      {
        instances = 4;
        anxiety_hijack = true; /* the V2 hijack only exists on top of the Anxiety OD one */
      }
      else
        die("--instances must be 2 or 4, got %s", v);
    }
    else if (strcmp(argv[i], "--anxiety-hijack") == 0)
      anxiety_hijack = true;
    else if (strcmp(argv[i], "--no-nam-pedals") == 0)
      no_nam_pedals = true;
    else if (strcmp(argv[i], "--page-pedal") == 0)
      page_pedal = true;
    else if (strcmp(argv[i], "--keep-work-dir") == 0)
      keep_work_dir = true;
    else
      die("unknown argument: %s", argv[i]);
  }

  if (!model_name)
    die("--model is required (pedalboard|mx5|gigboard) -- no default target");

  /* only the Pedalboard has the NAM pedals; the other models keep the original (Anxiety OD) design */
  NamPatchOptions opts = {0};
  opts.nam_pedals = strcmp(model_name, "pedalboard") == 0 && !no_nam_pedals;
  opts.anxiety_hijack = anxiety_hijack || !opts.nam_pedals;
  opts.anxiety_v2 = instances == 4;
  opts.page_pedal = page_pedal;
  if (instances == 4 && !opts.anxiety_hijack)
    die("--instances 4 needs the Anxiety OD hijack");

  char workdir[NAM_TEMPDIR_MAX_PATH];
  char tempdir_err[256];
  if (!nam_make_temp_dir(workdir, sizeof(workdir), tempdir_err, sizeof(tempdir_err)))
    die("creating work directory: %s", tempdir_err);
  printf("working directory: %s\n", workdir);

  size_t input_len;
  uint8_t* input_data = read_whole_file(input_img, &input_len);

  uint8_t* out_data;
  size_t out_len;
  char err[1024];
  bool ok = nam_patch_pipeline(input_data, input_len, model_name, &opts, workdir, print_progress, NULL, &out_data,
                                &out_len, err, sizeof(err));
  free(input_data);

  if (!ok)
  {
    if (!keep_work_dir)
      nam_remove_dir_recursive(workdir);
    die("%s", err);
  }

  write_whole_file(output_img, out_data, out_len);
  printf("OK  wrote %s (%zu bytes)\n", output_img, out_len);
  free(out_data);

  if (keep_work_dir)
    printf("--keep-work-dir: build directory left at %s\n", workdir);
  else
    nam_remove_dir_recursive(workdir);

  if (opts.nam_pedals)
    printf("\nOK  NAM pedals added (\"NAM\" and \"NAM 2\" in the Distortion list).\n");
  if (opts.anxiety_hijack)
    printf("%sOK  Anxiety OD (v1)%s process() NAM hijack applied.\n", opts.nam_pedals ? "" : "\n",
           opts.anxiety_v2 ? " + Anxiety OD V2" : "");
  if (page_pedal)
    printf("OK  Footswitch page-turner installed (FS5/FS11 -> USB Page Down/Up, see docs/PAGE_PEDAL.md).\n");
  return 0;
}
