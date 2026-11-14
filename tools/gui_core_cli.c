/* gui_core_cli.c -- milestone-1 CLI: built entirely on core/ (no debugfs/
 * e2fsck/xz/mkimage/python3 subprocess calls). Thin wrapper around
 * core/patch_pipeline.c -- the GUI app calls the exact same pipeline
 * function, see app/. Work-directory handling is portable (see
 * core/tempdir.h). Local dev/debugging tool only -- not built or shipped
 * by CI, see docs/BUILDING.md.
 *
 * usage: gui-core-cli <stock.img> <out.img> --model pedalboard|mx5|gigboard
 *                      [--mods LIST] [--anxiety-hijack] [--instances 2|4] [--no-nam-pedals]
 *                      [--page-pedal [--page-down-note N] [--page-up-note N]] [--keep-work-dir]
 *        gui-core-cli --list-mods
 *
 * Without --mods, the model's default is installed: the "NAM" and "NAM TWO"
 * pedals on the Pedalboard, the Anxiety OD hijack elsewhere. --mods takes an
 * exact comma-separated list of mod keys instead (see --list-mods). The other
 * flags adjust either one: --anxiety-hijack adds the Anxiety OD hijack,
 * --instances 4 adds the Anxiety OD V2 hijack too, --no-nam-pedals drops the
 * NAM pedals (without --mods, the Anxiety OD hijack replaces them), and
 * --page-pedal adds the footswitch page-turner.
 */
#include "mods.h"
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

static void list_mods(void)
{
  printf("Available mods (--mods takes a comma-separated list of keys):\n\n");
  for (int i = 0; i < NAM_MOD_COUNT; ++i)
  {
    const NamModInfo* m = &NAM_MODS[i];
    printf("  %-14s %s\n", m->key, m->title);
    printf("  %-14s %s\n", "", m->description);
    printf("  %-14s models: %s", "", nam_mod_models_str(m));
    if (m->requires != NAM_MOD_COUNT)
      printf("; requires: %s", NAM_MODS[m->requires].key);
    printf("\n\n");
  }
}

static void parse_mods(const char* list, NamModSelection* sel)
{
  memset(sel, 0, sizeof(*sel));
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", list);
  for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
  {
    const NamModInfo* m = nam_mod_by_key(tok);
    if (!m)
      die("unknown mod '%s' (see --list-mods)", tok);
    sel->enabled[m->id] = true;
  }
}

int main(int argc, char** argv)
{
  for (int i = 1; i < argc; ++i)
    if (strcmp(argv[i], "--list-mods") == 0)
    {
      list_mods();
      return 0;
    }

  const char* usage = "usage: %s <stock.img> <out.img> --model pedalboard|mx5|gigboard [--mods LIST] "
                      "[--anxiety-hijack] [--instances 2|4] [--no-nam-pedals] [--page-pedal [--page-down-note N] "
                      "[--page-up-note N]] [--keep-work-dir]\n       %s --list-mods";
  if (argc < 3)
    die(usage, argv[0], argv[0]);

  const char* input_img = argv[1];
  const char* output_img = argv[2];
  const char* model_name = NULL;
  bool keep_work_dir = false;
  const char* mods_list = NULL;
  int instances = 2;
  bool page_pedal = false;
  bool anxiety_hijack = false;
  bool no_nam_pedals = false;
  int page_down_note = -1, page_up_note = -1; /* -1 = the default */

  for (int i = 3; i < argc; ++i)
  {
    if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
      model_name = argv[++i];
    else if (strcmp(argv[i], "--mods") == 0 && i + 1 < argc)
      mods_list = argv[++i];
    else if (strcmp(argv[i], "--instances") == 0 && i + 1 < argc)
    {
      const char* v = argv[++i];
      if (strcmp(v, "2") == 0)
        instances = 2;
      else if (strcmp(v, "4") == 0)
        instances = 4;
      else
        die("--instances must be 2 or 4, got %s", v);
    }
    else if (strcmp(argv[i], "--anxiety-hijack") == 0)
      anxiety_hijack = true;
    else if (strcmp(argv[i], "--no-nam-pedals") == 0)
      no_nam_pedals = true;
    else if (strcmp(argv[i], "--page-pedal") == 0)
      page_pedal = true;
    else if (strcmp(argv[i], "--page-down-note") == 0 && i + 1 < argc)
      page_down_note = atoi(argv[++i]);
    else if (strcmp(argv[i], "--page-up-note") == 0 && i + 1 < argc)
      page_up_note = atoi(argv[++i]);
    else if (strcmp(argv[i], "--keep-work-dir") == 0)
      keep_work_dir = true;
    else
      die("unknown argument: %s", argv[i]);
  }

  if (!model_name)
    die("--model is required (pedalboard|mx5|gigboard) -- no default target");

  NamPatchOptions opts;
  memset(&opts, 0, sizeof(opts));
  NamModSelection* mods = &opts.mods;
  if (mods_list)
    parse_mods(mods_list, mods);
  else
    *mods = nam_mods_default(model_name);
  if (anxiety_hijack || instances == 4)
    mods->enabled[NAM_MOD_ANXIETY] = true; /* the V2 hijack only exists on top of the Anxiety OD one */
  if (instances == 4)
    mods->enabled[NAM_MOD_ANXIETY_V2] = true;
  if (no_nam_pedals)
  {
    mods->enabled[NAM_MOD_NAM_PEDALS] = false;
    if (!mods_list) /* --no-nam-pedals on its own means "hijack only" */
      mods->enabled[NAM_MOD_ANXIETY] = true;
  }
  if (page_pedal)
    mods->enabled[NAM_MOD_PAGE_PEDAL] = true;
  if (page_down_note >= 0 || page_up_note >= 0)
  {
    if (!mods->enabled[NAM_MOD_PAGE_PEDAL])
      die("--page-down-note / --page-up-note need the page-turner (--page-pedal)");
    opts.page_notes_set = true;
    opts.page_down_note = page_down_note >= 0 ? page_down_note : 10;
    opts.page_up_note = page_up_note >= 0 ? page_up_note : 4;
  }

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

  printf("\nOK  applied mods: %s\n", nam_mods_selection_str(mods));
  if (mods->enabled[NAM_MOD_NAM_PEDALS])
    printf("    NAM pedals: \"NAM\" and \"NAM TWO\" in the Distortion list, two instances each.\n");
  if (mods->enabled[NAM_MOD_PAGE_PEDAL])
    printf("    Footswitch page-turner: FS%d -> Page Down, FS%d -> Page Up (see docs/PAGE_PEDAL.md).\n",
           (opts.page_notes_set ? opts.page_down_note : 10) + 1, (opts.page_notes_set ? opts.page_up_note : 4) + 1);
  return 0;
}
