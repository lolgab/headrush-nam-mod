/* main.c -- Nuklear/SDL2 GUI wrapping core/. Downloads the stock
 * HeadRush firmware updater and produces a ready-to-run result:
 *   - macOS: downloads the Mac updater zip (plain zip) and produces a
 *     patched, ad-hoc-codesigned copy of the whole .app bundle (milestone 4
 *     -- see app/mac_package.c).
 *   - Windows: downloads the Windows updater zip and repacks its embedded
 *     7z SFX archive with the patched Update.img (milestone 5 -- see
 *     app/win_repack.c). Builds and runs on real Windows via a mingw-w64
 *     cross-compiled libext2fs (no vcpkg/MSYS2 binary package exists, but
 *     the e2fsprogs source builds cleanly -- see
 *     scripts/build_e2fsprogs_windows.sh and docs/BUILDING.md).
 *   - Other OSes: a plain Update_nam.img (milestone 3's "simplest output
 *     path").
 */
#include <SDL.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_RENDERER_IMPLEMENTATION
#include "nuklear.h"
#include "nuklear_sdl_renderer.h"

#include "http_download.h"
#include "model_targets.h"
#include "patch_pipeline.h"
#include "tempdir.h"
#include "zip_reader.h"

#ifdef __APPLE__
#include "mac_package.h"
#endif
#ifdef _WIN32
#include "win_repack.h"
#include <windows.h>
#include <shellapi.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The Mac updater zip is a plain zip (no 7z SFX like the Windows one), so
 * it's the simplest download path for this milestone regardless of which
 * OS the GUI itself runs on. */
static const char* const MAC_UPDATER_URLS[NAM_MODEL_COUNT] = {
  "https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/Pedalboard%20v2.7/MacOS%20Updater/HeadRush%20Pedalboard%202.7%20Firmware%20Updater%20-%20Mac.zip",
  "https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/MX5%20v2.7/MacOS%20Updater/HeadRush%20MX5%202.7%20Firmware%20Updater%20-%20Mac.zip",
  "https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/Gigboard%20v2.7/MacOS%20Updater/HeadRush%20Gigboard%202.7%20Firmware%20Updater%20-%20Mac.zip",
};

#ifdef _WIN32
/* An outer plain zip wrapping the 7z-SFX .exe installer itself. */
static const char* const WINDOWS_UPDATER_URLS[NAM_MODEL_COUNT] = {
  "https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/Pedalboard%20v2.7/Windows%20Updater/HeadRush%20Pedalboard%202.7%20Firmware%20Updater%20-%20Win.exe.zip",
  "https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/MX5%20v2.7/Windows%20Updater/HeadRush%20MX5%202.7%20Firmware%20Updater%20-%20Win.exe.zip",
  "https://cdn.inmusicbrands.com/HeadRush/FW/Aug24_Firmware_Updates/Gigboard%20v2.7/Windows%20Updater/HeadRush%20Gigboard%202.7%20Firmware%20Updater%20-%20Win.exe.zip",
};
#endif

#define MAX_LOG_LINES 128

typedef enum
{
  APP_STATE_PICK,
  APP_STATE_RUNNING,
  APP_STATE_DONE,
  APP_STATE_ERROR
} AppState;

typedef struct
{
  SDL_mutex* mutex;
  AppState state;
  int selected_model;
  char log_lines[MAX_LOG_LINES][256];
  int log_count;
  uint64_t dl_downloaded, dl_total;
  bool downloading;
  char output_path[900]; /* matches out_path's declared size in worker_main, no truncation possible */
  char error_msg[1024];
  bool cancel_requested;
} AppShared;

static void shared_push_log(AppShared* shared, const char* line)
{
  SDL_LockMutex(shared->mutex);
  if (shared->log_count < MAX_LOG_LINES)
  {
    snprintf(shared->log_lines[shared->log_count], sizeof(shared->log_lines[0]), "%s", line);
    shared->log_count++;
  }
  SDL_UnlockMutex(shared->mutex);
}

static void progress_to_log(const char* message, void* user_data)
{
  shared_push_log((AppShared*)user_data, message);
}

static bool download_progress_cb(uint64_t downloaded, uint64_t total, void* user_data)
{
  AppShared* shared = (AppShared*)user_data;
  SDL_LockMutex(shared->mutex);
  shared->dl_downloaded = downloaded;
  shared->dl_total = total;
  bool cancel = shared->cancel_requested;
  SDL_UnlockMutex(shared->mutex);
  return !cancel;
}

static void set_error(AppShared* shared, const char* msg)
{
  SDL_LockMutex(shared->mutex);
  snprintf(shared->error_msg, sizeof(shared->error_msg), "%s", msg);
  shared->state = APP_STATE_ERROR;
  SDL_UnlockMutex(shared->mutex);
}

static uint8_t* read_whole_file(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0)
  {
    fclose(f);
    return NULL;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz ? (size_t)sz : 1);
  if (buf && sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz)
  {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  if (buf)
    *out_len = (size_t)sz;
  return buf;
}

static bool write_whole_file_local(const char* path, const uint8_t* data, size_t len)
{
  FILE* f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
  fclose(f);
  return ok;
}

/* Quotes `path` for a POSIX /bin/sh -c "..." command line (macOS/Linux):
 * wraps in single quotes, escaping any embedded single quote as '\''. */
static void posix_shell_quote(const char* path, char* out, size_t out_size)
{
  size_t o = 0;
  if (o + 1 < out_size)
    out[o++] = '\'';
  for (const char* p = path; *p && o + 5 < out_size; ++p)
  {
    if (*p == '\'')
    {
      const char* rep = "'\\''";
      while (*rep && o + 1 < out_size)
        out[o++] = *rep++;
    }
    else
      out[o++] = *p;
  }
  if (o + 1 < out_size)
    out[o++] = '\'';
  out[o < out_size ? o : out_size - 1] = '\0';
}

#ifdef _WIN32
/* Quotes `path` for a cmd.exe command line: wraps in double quotes.
 * Our own generated paths never contain embedded double quotes. */
static void windows_shell_quote(const char* path, char* out, size_t out_size)
{
  snprintf(out, out_size, "\"%s\"", path);
}
#endif

/* Opens `path` with the OS's default handler for it (the .app/.exe
 * installer, or the plain .img on Linux -- whatever the OS does with it). */
static void nam_open_path(const char* path)
{
#if defined(__APPLE__)
  char quoted[1024];
  char cmd[1200];
  posix_shell_quote(path, quoted, sizeof(quoted));
  snprintf(cmd, sizeof(cmd), "open %s", quoted);
  (void)system(cmd); /* best-effort UI convenience -- nothing to recover from if it fails */
#elif defined(_WIN32)
  /* Deliberately NOT system()/cmd.exe -- same bug class fixed for 7z in
   * win_repack.c's run_7z(): cmd.exe's command-lookup heuristic mangles
   * command lines once a path has both spaces and parens (every output
   * path here does, e.g. "...Firmware Updater (NAM mod).exe"), so the
   * old "start \"\" <path>" silently failed. ShellExecuteA takes the
   * path as its own parameter, never as part of a parsed command line,
   * so it isn't subject to that parser at all. */
  ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
  char quoted[1024];
  char cmd[1200];
  posix_shell_quote(path, quoted, sizeof(quoted));
  snprintf(cmd, sizeof(cmd), "xdg-open %s", quoted);
  (void)system(cmd);
#endif
}

/* Reveals `path` in the OS's file manager (Finder/Explorer), selecting it
 * where the platform supports that; falls back to just opening the
 * containing folder on Linux. */
static void nam_reveal_path(const char* path)
{
#if defined(__APPLE__)
  char quoted[1024];
  char cmd[1200];
  posix_shell_quote(path, quoted, sizeof(quoted));
  snprintf(cmd, sizeof(cmd), "open -R %s", quoted);
  (void)system(cmd);
#elif defined(_WIN32)
  /* Deliberately NOT system()/cmd.exe -- same bug class fixed for 7z in
   * win_repack.c's run_7z(): once `path` has spaces + parens, cmd.exe's
   * command-lookup heuristic mangles "explorer /select,<path>" and
   * either no-ops or drops the /select argument, which is why this
   * button opened Explorer to the desktop instead of selecting the
   * file. CreateProcessA launches explorer.exe directly, bypassing
   * cmd.exe's parser entirely (explorer's own /select parsing is
   * unchanged and already correct). */
  char quoted[1024];
  windows_shell_quote(path, quoted, sizeof(quoted));
  char cmdline[1300];
  snprintf(cmdline, sizeof(cmdline), "explorer.exe /select,%s", quoted);

  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));
  if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
  {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
#else
  char quoted[1024];
  char cmd[1200];
  char dir[900];
  snprintf(dir, sizeof(dir), "%s", path);
  char* slash = strrchr(dir, '/');
  if (slash)
    *slash = '\0';
  else
    snprintf(dir, sizeof(dir), ".");
  posix_shell_quote(dir, quoted, sizeof(quoted));
  snprintf(cmd, sizeof(cmd), "xdg-open %s", quoted);
  (void)system(cmd);
#endif
}

static int worker_main(void* data)
{
  AppShared* shared = (AppShared*)data;

  int model_index;
  SDL_LockMutex(shared->mutex);
  model_index = shared->selected_model;
  shared->downloading = true;
  SDL_UnlockMutex(shared->mutex);

  const ModelTarget* target = &NAM_MODEL_TARGETS[model_index];
  char msg[256];
  snprintf(msg, sizeof(msg), "Downloading stock %s firmware...", target->name);
  shared_push_log(shared, msg);

  uint8_t* zip_data;
  size_t zip_len;
  char err[1024];
#ifdef _WIN32
  const char* download_url = WINDOWS_UPDATER_URLS[model_index];
#else
  const char* download_url = MAC_UPDATER_URLS[model_index];
#endif
  if (!nam_http_download(download_url, download_progress_cb, shared, &zip_data, &zip_len, err, sizeof(err)))
  {
    set_error(shared, err);
    return 1;
  }

  SDL_LockMutex(shared->mutex);
  shared->downloading = false;
  SDL_UnlockMutex(shared->mutex);
  shared_push_log(shared, "OK  downloaded stock firmware updater");

  char workdir[NAM_TEMPDIR_MAX_PATH];
  char tempdir_err[256];
  if (!nam_make_temp_dir(workdir, sizeof(workdir), tempdir_err, sizeof(tempdir_err)))
  {
    set_error(shared, tempdir_err);
    free(zip_data);
    return 1;
  }

  uint8_t* stock_img = NULL;
  size_t stock_img_len = 0;
#ifdef __APPLE__
  char extract_dir[700];
  snprintf(extract_dir, sizeof(extract_dir), "%s/extracted", workdir);
  bool ok = nam_zip_extract_all(zip_data, zip_len, extract_dir, err, sizeof(err));
  free(zip_data);
  if (!ok)
  {
    set_error(shared, err);
    return 1;
  }

  char app_dir[700];
  if (!nam_mac_find_app_dir(extract_dir, app_dir, sizeof(app_dir)))
  {
    set_error(shared, "no *.app bundle found in the stock updater zip -- HeadRush may have changed the layout");
    return 1;
  }

  char stock_img_path[900];
  if (!nam_mac_find_update_img(app_dir, stock_img_path, sizeof(stock_img_path)))
  {
    set_error(shared, "no Update.img found inside the stock .app -- HeadRush may have changed the layout");
    return 1;
  }
  stock_img = read_whole_file(stock_img_path, &stock_img_len);
  if (!stock_img)
  {
    set_error(shared, "reading the stock Update.img failed");
    return 1;
  }
  shared_push_log(shared, "OK  extracted the stock updater .app");
#elif defined(_WIN32)
  /* Never rely on a "7z" on PATH -- most end users have no 7-Zip CLI
   * installed at all (even those with the 7-Zip GUI usually don't have it
   * on PATH). The release zip ships 7z.exe + 7z.dll next to this exe;
   * resolve them by this exe's own directory instead. */
  char sevenzip_path[MAX_PATH];
  {
    DWORD n = GetModuleFileNameA(NULL, sevenzip_path, sizeof(sevenzip_path));
    char* slash = (n > 0 && n < sizeof(sevenzip_path)) ? strrchr(sevenzip_path, '\\') : NULL;
    if (!slash)
    {
      set_error(shared, "couldn't determine this exe's own directory (GetModuleFileNameA failed)");
      return 1;
    }
    snprintf(slash + 1, sizeof(sevenzip_path) - (size_t)(slash + 1 - sevenzip_path), "7z.exe");
  }

  uint8_t* exe_data;
  size_t exe_len;
  bool ok = nam_zip_extract_by_suffix(zip_data, zip_len, ".exe", &exe_data, &exe_len, err, sizeof(err));
  free(zip_data);
  if (!ok)
  {
    set_error(shared, err);
    return 1;
  }

  char stock_exe_path[700];
  snprintf(stock_exe_path, sizeof(stock_exe_path), "%s/stock_updater.exe", workdir);
  bool wrote = write_whole_file_local(stock_exe_path, exe_data, exe_len);
  free(exe_data);
  if (!wrote)
  {
    set_error(shared, "writing the stock updater .exe failed");
    return 1;
  }

  if (!nam_win_extract_stock_img(stock_exe_path, sevenzip_path, workdir, &stock_img, &stock_img_len, err,
                                  sizeof(err)))
  {
    set_error(shared, err);
    return 1;
  }
  shared_push_log(shared, "OK  extracted Update.img from the stock updater .exe");
#else
  bool ok = nam_zip_extract_by_suffix(zip_data, zip_len, "Update.img", &stock_img, &stock_img_len, err, sizeof(err));
  free(zip_data);
  if (!ok)
  {
    set_error(shared, err);
    return 1;
  }
  shared_push_log(shared, "OK  extracted Update.img from the stock updater");
#endif

  uint8_t* out_data;
  size_t out_len;
  ok = nam_patch_pipeline(stock_img, stock_img_len, target->name, workdir, progress_to_log, shared, &out_data,
                          &out_len, err, sizeof(err));
  free(stock_img);

  if (!ok)
  {
    nam_remove_dir_recursive(workdir);
    set_error(shared, err);
    return 1;
  }

  char cwd[512];
  if (!getcwd(cwd, sizeof(cwd)))
    snprintf(cwd, sizeof(cwd), ".");
  char out_path[900];

#ifdef __APPLE__
  char app_base_name[512];
  {
    const char* slash = strrchr(app_dir, '/');
    snprintf(app_base_name, sizeof(app_base_name), "%s", slash ? slash + 1 : app_dir);
    size_t n = strlen(app_base_name);
    if (n > 4 && strcmp(app_base_name + n - 4, ".app") == 0)
      app_base_name[n - 4] = '\0';
  }
  snprintf(out_path, sizeof(out_path), "%s/%s (NAM mod).app", cwd, app_base_name);
  bool packaged = nam_mac_package_app(extract_dir, out_data, out_len, out_path, err, sizeof(err));
  free(out_data);
  if (!packaged)
  {
    nam_remove_dir_recursive(workdir);
    set_error(shared, err);
    return 1;
  }
  if (err[0] != '\0')
    shared_push_log(shared, err); /* non-fatal codesign warning, if any */

  /* Keep an unmodified copy of the stock updater alongside the patched
   * one, for recovery -- the old (now-deleted) quickstart_mac.sh did
   * this too before the C/SDL2 rewrite dropped it. Best-effort: nothing
   * to recover from if it fails, the patched output above is what
   * matters. */
  {
    char stock_out_path[900];
    snprintf(stock_out_path, sizeof(stock_out_path), "%s/%s (stock).app", cwd, app_base_name);
    char q_app[1300], q_stock_out[1300];
    posix_shell_quote(app_dir, q_app, sizeof(q_app));
    posix_shell_quote(stock_out_path, q_stock_out, sizeof(q_stock_out));
    char cpcmd[3000];
    snprintf(cpcmd, sizeof(cpcmd), "rm -rf %s && cp -R %s %s", q_stock_out, q_app, q_stock_out);
    if (system(cpcmd) == 0)
      shared_push_log(shared, "OK  saved unmodified stock updater alongside the patched one (for recovery)");
    else
      shared_push_log(shared, "note: couldn't save a copy of the unmodified stock updater (non-fatal)");
  }
#elif defined(_WIN32)
  snprintf(out_path, sizeof(out_path), "%s/HeadRush %s Firmware Updater (NAM mod).exe", cwd, target->name);
  bool repacked =
    nam_win_repack_updater(stock_exe_path, out_data, out_len, out_path, sevenzip_path, workdir, err, sizeof(err));
  free(out_data);
  if (!repacked)
  {
    nam_remove_dir_recursive(workdir);
    set_error(shared, err);
    return 1;
  }

  /* Keep an unmodified copy of the stock updater alongside the patched
   * one, for recovery -- the old (now-deleted) quickstart_windows.sh did
   * this too before the C/SDL2 rewrite dropped it. Best-effort: nothing
   * to recover from if it fails, the patched output above is what
   * matters. */
  {
    char stock_out_path[900];
    snprintf(stock_out_path, sizeof(stock_out_path), "%s/HeadRush %s Firmware Updater (stock).exe", cwd,
              target->name);
    if (CopyFileA(stock_exe_path, stock_out_path, FALSE))
      shared_push_log(shared, "OK  saved unmodified stock updater alongside the patched one (for recovery)");
    else
      shared_push_log(shared, "note: couldn't save a copy of the unmodified stock updater (non-fatal)");
  }
#else
  snprintf(out_path, sizeof(out_path), "%s/Update_nam.img", cwd);
  FILE* f = fopen(out_path, "wb");
  if (!f || (out_len > 0 && fwrite(out_data, 1, out_len, f) != out_len))
  {
    if (f)
      fclose(f);
    free(out_data);
    nam_remove_dir_recursive(workdir);
    set_error(shared, "writing Update_nam.img failed");
    return 1;
  }
  fclose(f);
  free(out_data);
#endif

  nam_remove_dir_recursive(workdir);

  SDL_LockMutex(shared->mutex);
  snprintf(shared->output_path, sizeof(shared->output_path), "%s", out_path);
  shared->state = APP_STATE_DONE;
  SDL_UnlockMutex(shared->mutex);
  return 0;
}

int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;

  nam_http_global_init();

  if (SDL_Init(SDL_INIT_VIDEO) != 0)
  {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* win = SDL_CreateWindow("HeadRush NAM Mod Installer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      640, 480, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  SDL_Renderer* renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  struct nk_context* ctx = nk_sdl_init(win, renderer);
  {
    struct nk_font_atlas* atlas;
    nk_sdl_font_stash_begin(&atlas);
    nk_sdl_font_stash_end();
  }

  AppShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.mutex = SDL_CreateMutex();
  shared.state = APP_STATE_PICK;
  shared.selected_model = 0;

  SDL_Thread* worker_thread = NULL;
  bool running = true;

  while (running)
  {
    SDL_Event evt;
    nk_input_begin(ctx);
    while (SDL_PollEvent(&evt))
    {
      if (evt.type == SDL_QUIT)
        running = false;
      nk_sdl_handle_event(&evt);
    }
    nk_input_end(ctx);

    SDL_LockMutex(shared.mutex);
    AppState state = shared.state;
    SDL_UnlockMutex(shared.mutex);

    if (nk_begin(ctx, "HeadRush NAM Mod Installer", nk_rect(0, 0, 640, 480), NK_WINDOW_NO_SCROLLBAR))
    {
      if (state == APP_STATE_PICK)
      {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "Pick your device, then click Install.", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "Downloads the official stock firmware and applies the NAM mod --", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "no terminal, no extra tools needed.", NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 30, 1);
        for (int i = 0; i < NAM_MODEL_COUNT; ++i)
        {
          int selected = (shared.selected_model == i);
          if (nk_option_label(ctx, NAM_MODEL_TARGETS[i].name, selected) && !selected)
            shared.selected_model = i;
        }

        nk_layout_row_dynamic(ctx, 40, 1);
        if (nk_button_label(ctx, "Install NAM Mod"))
        {
          SDL_LockMutex(shared.mutex);
          shared.state = APP_STATE_RUNNING;
          shared.log_count = 0;
          shared.cancel_requested = false;
          SDL_UnlockMutex(shared.mutex);
          worker_thread = SDL_CreateThread(worker_main, "nam-worker", &shared);
        }
      }
      else if (state == APP_STATE_RUNNING)
      {
        SDL_LockMutex(shared.mutex);
        bool downloading = shared.downloading;
        uint64_t dl_downloaded = shared.dl_downloaded, dl_total = shared.dl_total;
        int log_count = shared.log_count;
        char lines[MAX_LOG_LINES][256];
        memcpy(lines, shared.log_lines, sizeof(lines));
        SDL_UnlockMutex(shared.mutex);

        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "Working...", NK_TEXT_LEFT);

        if (downloading && dl_total > 0)
        {
          nk_layout_row_dynamic(ctx, 24, 1);
          size_t prog = (size_t)((dl_downloaded * 100) / dl_total);
          nk_size prog_sz = (nk_size)prog;
          nk_progress(ctx, &prog_sz, 100, NK_FIXED);
        }

        nk_layout_row_dynamic(ctx, 260, 1);
        if (nk_group_begin(ctx, "log", NK_WINDOW_BORDER))
        {
          nk_layout_row_dynamic(ctx, 18, 1);
          for (int i = 0; i < log_count; ++i)
            nk_label(ctx, lines[i], NK_TEXT_LEFT);
          nk_group_end(ctx);
        }
      }
      else if (state == APP_STATE_DONE)
      {
        char path[900];
        SDL_LockMutex(shared.mutex);
        snprintf(path, sizeof(path), "%s", shared.output_path);
        SDL_UnlockMutex(shared.mutex);

        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "Done! NAM mod applied.", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "Wrote:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, path, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1);
#if defined(__APPLE__) || defined(_WIN32)
        nk_label(ctx, "Run this like the official updater, with your device in", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "firmware-update mode.", NK_TEXT_LEFT);
#else
        nk_label(ctx, "Flash this file the same way you'd flash the stock Update.img", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "on your device (device must be in firmware-update mode).", NK_TEXT_LEFT);
#endif
        nk_layout_row_dynamic(ctx, 30, 2);
        if (nk_button_label(ctx, "Open"))
          nam_open_path(path);
        if (nk_button_label(ctx, "Open in Folder"))
          nam_reveal_path(path);
      }
      else /* APP_STATE_ERROR */
      {
        char errmsg[1024];
        SDL_LockMutex(shared.mutex);
        snprintf(errmsg, sizeof(errmsg), "%s", shared.error_msg);
        SDL_UnlockMutex(shared.mutex);

        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label_colored(ctx, "Something went wrong:", NK_TEXT_LEFT, nk_rgb(220, 60, 60));
        nk_layout_row_dynamic(ctx, 80, 1);
        nk_label_wrap(ctx, errmsg);
        nk_layout_row_dynamic(ctx, 40, 1);
        if (nk_button_label(ctx, "Try Again"))
        {
          SDL_LockMutex(shared.mutex);
          shared.state = APP_STATE_PICK;
          SDL_UnlockMutex(shared.mutex);
        }
      }
    }
    nk_end(ctx);

    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
    SDL_RenderClear(renderer);
    nk_sdl_render(NK_ANTI_ALIASING_ON);
    SDL_RenderPresent(renderer);
  }

  if (worker_thread)
  {
    SDL_LockMutex(shared.mutex);
    shared.cancel_requested = true;
    SDL_UnlockMutex(shared.mutex);
    SDL_WaitThread(worker_thread, NULL);
  }
  SDL_DestroyMutex(shared.mutex);

  nk_sdl_shutdown();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(win);
  SDL_Quit();
  nam_http_global_cleanup();
  return 0;
}
