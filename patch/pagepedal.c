/* pagepedal.c -- on-device daemon: turns two of the Pedalboard's footswitches
 * into USB-keyboard Page Down / Page Up for the host computer.
 *
 * How it fits together
 * --------------------
 * The footswitch panel talks to Evil over (internal) MIDI: assignments in
 * /usr/Evil/Assignments/M-Audio_Pass_Through_Assignments.qml map Note On/Off,
 * channel 0, note 0..11 to RawFootswitches/FS1..FS12. Evil reads that MIDI
 * through ALSA sequencer. This daemon opens its own sequencer client,
 * subscribes to the panel's output port(s) (ALSA lets any number of clients
 * subscribe to one port, so Evil keeps working untouched) and, for the two
 * configured notes, writes standard 8-byte boot-keyboard reports to the
 * configfs HID gadget's /dev/hidgN, which the host sees as a plain keyboard.
 *
 * The footswitches themselves still reach Evil, so the two switches used here
 * should have no effect assigned in the HeadRush UI (a switch with nothing
 * assigned does nothing there), otherwise they'll do both things.
 *
 * Built with only kernel UAPI headers (no libasound): talks to /dev/snd/seq
 * with ioctls directly, so there is nothing to link against on the device.
 *
 * Config (optional) -- first existing of:
 *   /media/az01-internal/Evil/usb_mnt/PagePedal.conf   (editable from the
 *       device's USB-drive mode / File Manager)
 *   /usr/Evil/pagepedal.conf
 * key=value lines, '#' comments:
 *   page_down_note=10   note number (FS number - 1) that sends Page Down
 *   page_up_note=4      note number that sends Page Up
 *   channel=0           MIDI channel (0-based) of the panel's note messages
 *   learn=0             1 = calibration mode: footswitch FSn types a letter
 *                       ('a' = FS1 ... 'l' = FS12) so you can see, in any text
 *                       editor on the host, which switch is which note.
 *
 * Deliberately never writes to the USB-drive image (it is exported to the host
 * in mass-storage mode); diagnostics go to stderr, which the start script
 * sends to /tmp/pagepedal.log.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sound/asequencer.h>

#define KEY_PAGE_UP 0x4B
#define KEY_PAGE_DOWN 0x4E
#define KEY_LETTER_A 0x04

#define RESCAN_MS 2000
#define MAX_HIDG 4

typedef struct
{
  int page_down_note;
  int page_up_note;
  int channel;
  int learn;
} Config;

static const char* const CONFIG_PATHS[] = {
  "/media/az01-internal/Evil/usb_mnt/PagePedal.conf",
  "/usr/Evil/pagepedal.conf",
};

static volatile sig_atomic_t g_quit = 0;

static void on_signal(int sig)
{
  (void)sig;
  g_quit = 1;
}

static void logf_(const char* fmt, ...)
{
  char ts[32];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
  fprintf(stderr, "%s pagepedal: ", ts);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}

static long now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void config_defaults(Config* c)
{
  c->page_down_note = 10; /* FS11 */
  c->page_up_note = 4;    /* FS5  */
  c->channel = 0;
  c->learn = 0;
}

/* Returns mtime of the config file in use (0 if none), fills *c. */
static time_t config_load(Config* c)
{
  config_defaults(c);
  for (size_t i = 0; i < sizeof(CONFIG_PATHS) / sizeof(CONFIG_PATHS[0]); ++i)
  {
    FILE* f = fopen(CONFIG_PATHS[i], "r");
    if (!f)
      continue;
    char line[160];
    while (fgets(line, sizeof(line), f))
    {
      char* p = line;
      while (*p == ' ' || *p == '\t')
        ++p;
      if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r')
        continue;
      char* eq = strchr(p, '=');
      if (!eq)
        continue;
      *eq = '\0';
      char* key = p;
      char* val = eq + 1;
      for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); --e)
        e[-1] = '\0';
      int v = atoi(val);
      if (strcmp(key, "page_down_note") == 0)
        c->page_down_note = v;
      else if (strcmp(key, "page_up_note") == 0)
        c->page_up_note = v;
      else if (strcmp(key, "channel") == 0)
        c->channel = v;
      else if (strcmp(key, "learn") == 0)
        c->learn = v != 0;
    }
    fclose(f);
    struct stat st;
    if (stat(CONFIG_PATHS[i], &st) == 0)
      return st.st_mtime ? st.st_mtime : 1;
    return 1;
  }
  return 0;
}

/* ---- HID output ---------------------------------------------------------- */

static int g_hid_fd = -1;

static void hid_send(unsigned char key)
{
  if (g_hid_fd < 0)
  {
    char path[24];
    for (int i = 0; i < MAX_HIDG && g_hid_fd < 0; ++i)
    {
      snprintf(path, sizeof(path), "/dev/hidg%d", i);
      g_hid_fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
      if (g_hid_fd >= 0)
        logf_("opened %s", path);
    }
    if (g_hid_fd < 0)
      return; /* no gadget bound right now (audio/USB-drive mode, or no host) */
  }
  unsigned char report[8] = {0, 0, key, 0, 0, 0, 0, 0};
  ssize_t n = write(g_hid_fd, report, sizeof(report));
  if (n < 0 && errno != EAGAIN)
  {
    /* gadget torn down (ESHUTDOWN/ENODEV/...) -- reopen on the next press */
    logf_("hid write failed: %s", strerror(errno));
    close(g_hid_fd);
    g_hid_fd = -1;
  }
}

/* ---- ALSA sequencer (raw ioctls) ---------------------------------------- */

static int seq_open(int* my_client)
{
  int fd = open("/dev/snd/seq", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return -1;

  int cid = -1;
  if (ioctl(fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &cid) < 0)
  {
    close(fd);
    return -1;
  }

  struct snd_seq_client_info ci;
  memset(&ci, 0, sizeof(ci));
  ci.client = cid;
  ci.type = USER_CLIENT;
  snprintf(ci.name, sizeof(ci.name), "pagepedal");
  if (ioctl(fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &ci) < 0)
  {
    close(fd);
    return -1;
  }

  struct snd_seq_port_info pi;
  memset(&pi, 0, sizeof(pi));
  pi.addr.client = cid;
  pi.addr.port = 0;
  snprintf(pi.name, sizeof(pi.name), "pagepedal in");
  pi.capability = SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
  pi.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
  pi.midi_channels = 16;
  if (ioctl(fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &pi) < 0)
  {
    close(fd);
    return -1;
  }
  *my_client = cid;
  return fd;
}

static void seq_subscribe(int fd, int sender_client, int sender_port, int my_client)
{
  struct snd_seq_port_subscribe sub;
  memset(&sub, 0, sizeof(sub));
  sub.sender.client = (unsigned char)sender_client;
  sub.sender.port = (unsigned char)sender_port;
  sub.dest.client = (unsigned char)my_client;
  sub.dest.port = 0;
  if (ioctl(fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) == 0)
    logf_("subscribed to %d:%d", sender_client, sender_port);
  /* EBUSY = already subscribed: expected on every rescan, stay quiet */
}

/* Subscribes to the readable ports of every client that could be the panel.
 * Prefers kernel (hardware) clients; only if there are none besides the
 * built-in "Midi Through" does it fall back to other userspace clients. */
static void seq_scan(int fd, int my_client, int pass_user_clients)
{
  struct snd_seq_client_info ci;
  memset(&ci, 0, sizeof(ci));
  ci.client = -1;
  while (ioctl(fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT, &ci) == 0)
  {
    int client = ci.client;
    if (client == my_client || client == SNDRV_SEQ_CLIENT_SYSTEM || client == 14 /* Midi Through */)
      continue;
    if (!pass_user_clients && ci.type != KERNEL_CLIENT)
      continue;

    struct snd_seq_port_info pi;
    memset(&pi, 0, sizeof(pi));
    pi.addr.client = client;
    pi.addr.port = -1;
    while (ioctl(fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT, &pi) == 0)
    {
      unsigned need = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ;
      if ((pi.capability & need) == need && !(pi.capability & SNDRV_SEQ_PORT_CAP_NO_EXPORT))
        seq_subscribe(fd, client, pi.addr.port, my_client);
    }
  }
}

static int count_kernel_clients(int fd, int my_client)
{
  int n = 0;
  struct snd_seq_client_info ci;
  memset(&ci, 0, sizeof(ci));
  ci.client = -1;
  while (ioctl(fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT, &ci) == 0)
  {
    if (ci.client != my_client && ci.client != SNDRV_SEQ_CLIENT_SYSTEM && ci.client != 14 && ci.type == KERNEL_CLIENT)
      ++n;
  }
  return n;
}

static void handle_note(const Config* cfg, int note, int pressed)
{
  unsigned char key = 0;
  if (cfg->learn)
  {
    if (note >= 0 && note < 12)
      key = (unsigned char)(KEY_LETTER_A + note);
  }
  else if (note == cfg->page_down_note)
    key = KEY_PAGE_DOWN;
  else if (note == cfg->page_up_note)
    key = KEY_PAGE_UP;

  if (!key)
    return;
  logf_("note %d %s -> key 0x%02x", note, pressed ? "down" : "up", pressed ? key : 0);
  hid_send(pressed ? key : 0);
}

static void process_events(const Config* cfg, const unsigned char* buf, size_t len)
{
  size_t off = 0;
  while (off + sizeof(struct snd_seq_event) <= len)
  {
    const struct snd_seq_event* ev = (const struct snd_seq_event*)(buf + off);
    size_t step = sizeof(*ev);
    if ((ev->flags & SNDRV_SEQ_EVENT_LENGTH_MASK) == SNDRV_SEQ_EVENT_LENGTH_VARIABLE)
      step += ev->data.ext.len; /* SysEx etc. -- payload follows, skip it */

    switch (ev->type)
    {
      case SNDRV_SEQ_EVENT_NOTEON:
      case SNDRV_SEQ_EVENT_NOTEOFF:
        if (ev->data.note.channel == cfg->channel)
        {
          int pressed = ev->type == SNDRV_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0;
          handle_note(cfg, ev->data.note.note, pressed);
        }
        break;
      default:
        break;
    }
    if (step > len - off)
      break;
    off += step;
  }
}

int main(void)
{
  int lock = open("/run/pagepedal.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (lock < 0)
    lock = open("/tmp/pagepedal.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0)
    return 0; /* already running */

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  Config cfg;
  time_t cfg_mtime = config_load(&cfg);
  logf_("start: page_down_note=%d page_up_note=%d channel=%d learn=%d (config %s)", cfg.page_down_note,
        cfg.page_up_note, cfg.channel, cfg.learn, cfg_mtime ? "found" : "not found, defaults");

  while (!g_quit)
  {
    int my_client = -1;
    int fd = seq_open(&my_client);
    if (fd < 0)
    {
      sleep(2); /* sequencer not up yet */
      continue;
    }
    logf_("sequencer open, client %d", my_client);

    /* hear about hot-plugged ports: System:Announce -> us */
    seq_subscribe(fd, SNDRV_SEQ_CLIENT_SYSTEM, SNDRV_SEQ_PORT_SYSTEM_ANNOUNCE, my_client);

    int fallback_logged = 0;
    long next_scan_ms = 0;
    for (;;)
    {
      long now = now_ms();
      if (now >= next_scan_ms)
      {
        next_scan_ms = now + RESCAN_MS;
        int have_kernel = count_kernel_clients(fd, my_client);
        if (!have_kernel && !fallback_logged)
        {
          logf_("no kernel MIDI client yet, also trying userspace ones");
          fallback_logged = 1;
        }
        seq_scan(fd, my_client, !have_kernel);

        /* pick up config edits */
        Config fresh;
        time_t m = config_load(&fresh);
        if (m && m != cfg_mtime)
        {
          cfg = fresh;
          cfg_mtime = m;
          logf_("config reloaded: page_down_note=%d page_up_note=%d channel=%d learn=%d", cfg.page_down_note,
                cfg.page_up_note, cfg.channel, cfg.learn);
        }
      }

      int quit_inner = 0;
      struct pollfd pfd = {.fd = fd, .events = POLLIN};
      long wait = next_scan_ms - now_ms();
      int r = poll(&pfd, 1, wait < 0 ? 0 : (int)wait);
      if (g_quit)
        break;
      if (r > 0)
      {
        unsigned char buf[2048];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0)
          process_events(&cfg, buf, (size_t)n);
        else if (n < 0 && errno != EAGAIN && errno != EINTR)
          quit_inner = 1;
        /* drain whatever else is pending before rescanning */
        while (!g_quit)
        {
          struct pollfd p2 = {.fd = fd, .events = POLLIN};
          if (poll(&p2, 1, 0) <= 0)
            break;
          n = read(fd, buf, sizeof(buf));
          if (n <= 0)
            break;
          process_events(&cfg, buf, (size_t)n);
        }
      }
      else if (r < 0 && errno != EINTR)
        quit_inner = 1;
      if (quit_inner)
        break;
    }
    close(fd);
    if (!g_quit)
      sleep(1);
  }

  hid_send(0);
  return 0;
}
