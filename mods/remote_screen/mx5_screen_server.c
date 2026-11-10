/*
 * mx5_screen_server.c -- stream the HeadRush MX5's screen over a USB serial
 * channel and inject touch events back, giving a remote view/control of the
 * device from a PC.
 *
 * Runs on the device (ARM, glibc). Talks over a second CDC-ACM gadget channel
 * (/dev/ttyGS1) so the debug console on /dev/ttyGS0 stays usable.
 *
 * WHY THIS WORKS (measured on real hardware, MX5 firmware 2.7)
 * -----------------------------------------------------------
 *  - /dev/fb0 ("rockchipdrmfb", DRM fbdev emulation) really does hold the live
 *    composited screen: two grabs across a UI change differ, and the pixels
 *    match Qt's own background colour. Qt uses the eglfs_mali backend, which
 *    presents through fbdev -- had it been eglfs_kms, this would not work.
 *  - Geometry is 480x800 at 32bpp, TRIPLE buffered (virtual 480x2400), so the
 *    visible buffer is chosen by vinfo.yoffset and changes frame to frame.
 *  - The panel is mounted rotated: the framebuffer is portrait 480x800 while
 *    the UI is landscape 800x480. Rotation is left to the viewer.
 *  - The CDC-ACM channel sustains ~12 MB/s (USB high-speed), i.e. ~8 fps of
 *    completely uncompressed frames -- so tile-delta alone is plenty and no
 *    image codec is needed on the device.
 *
 * PROTOCOL (little-endian, device -> host). EVERY frame is self-describing, so
 * a viewer can attach, detach and re-attach at any time without a handshake --
 * the transport is a serial port with no connection semantics, so there is no
 * way for the server to know when a client appears.
 *   frame header (24 bytes):
 *     "MX5S" u16 ver, u16 tile, u32 width, u32 height, u32 bpp, u32 ntiles
 *   then per tile: u16 tx, u16 ty, u8 enc, u32 len, then len bytes
 *     enc 0 = raw BGRX (len == tile*tile*4)
 *     enc 1 = RLE: repeated { u16 count, u32 pixel }, count pixels each
 *   RLE matters because this UI is mostly flat colour: a full-screen keyframe
 *   drops from ~1.5 MB to a small fraction of that, which is the difference
 *   between overflowing the host's serial buffer and not. Tiles that don't
 *   compress fall back to raw, so the payload is never larger than raw.
 *   Only tiles that changed since the last frame are sent, so an idle screen
 *   costs nothing. A client resyncs by scanning for the "MX5S" magic.
 *
 * PROTOCOL (host -> device):
 *   'T' u8 down, u16 x, u16 y   -- 6 bytes; x,y in FRAMEBUFFER coords (480x800)
 *   'E' i8 delta               -- 2 bytes; turn the encoder by `delta` clicks
 *                                  (negative = left). Injected as MIDI, see below.
 *   'P' u8 down                 -- 2 bytes; press/release the encoder knob.
 *   'K'                         -- 1 byte; request a full (keyframe) update and
 *                                  reset flow-control credit. Sent by a client
 *                                  on connect: a fresh client has no baseline to
 *                                  apply deltas to.
 *   'A'                         -- 1 byte; ack, "I finished a frame".
 *
 * FLOW CONTROL. The server only sends a frame when it holds credit, granted by
 * the client's acks. Without this the device outruns the host, the backlog grows
 * without bound and latency climbs forever -- and delta frames cannot simply be
 * dropped to catch up, because each one depends on the last. Credit also means
 * we never stream into the void before a viewer attaches. While the server holds
 * no credit it keeps its shadow untouched, so changes accumulate and are
 * coalesced into the next frame it is allowed to send.
 *   A keyframe is also emitted every KEYFRAME_SEC as a safety net.
 *
 * Touch is injected through /dev/uinput as an absolute multitouch device, so
 * it looks like the real ili2116 touchscreen to Qt.
 *
 * ENCODER EMULATION
 * -----------------
 * The encoder is NOT an input device -- it is the control-surface MCU sending
 * MIDI over a serial link (snd-serdev-midi), which Evil consumes via the ALSA
 * sequencer. So uinput cannot reach it; we have to become a MIDI source.
 *
 * Evil creates one ALSA seq client per MIDI device, e.g.
 *     client 129: "Midi::In::HG04 Control Surface MIDI 1"  port 0  (-We-)
 * subscribed from the hardware (client 20). That port accepts writes, and Evil
 * attributes events by which of ITS ports they arrive on -- so events we send
 * there are indistinguishable from the real control surface, and the stock
 * assignment file (/usr/Evil/Assignments/HG04_Control_Surface_MIDI_1_
 * Assignments.qml) maps them for free:
 *     CC 3      -> JogOutput  -> /Engine/PushEncoderCtrl/Encoder     (turn)
 *     Note 4    -> PressAndHold -> /Engine/PushEncoderCtrl/EncoderEnter (push)
 * We locate the port by NAME rather than hardcoding 129, since client numbers
 * are assigned dynamically.
 *
 * libasound is dlopen()ed, so the server still builds and runs (without encoder
 * support) if it is missing.
 *
 * Scheduling: pin to a core away from audio. On this device CPU0 services the
 * I2S DMA IRQ (~4M interrupts) plus ~20 Evil threads, and CPU2 carries ~14
 * (the DSP pool); CPU3 has one. So: taskset -c 3 ./mx5_screen_server
 *
 * Build (from repo root) with the Bootlin glibc-2.31 toolchain in .toolchain,
 * e.g. see tools/mx5_remote_screen/build.sh
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TILE 32
#define PROTO_VER 3
/* Safety net only -- a viewer asks for a keyframe with 'K' when it attaches.
 * Keep this long: a full frame is ~1.5 MB raw, and firing it every few seconds
 * was enough to overrun the host's buffer and truncate frames. */
#define KEYFRAME_SEC 30
#define ENC_RAW 0
#define ENC_RLE 1

static int fb_fd = -1, ui_fd = -1, out_fd = -1;
static uint8_t *fbmem = NULL;
static size_t fbmem_len = 0;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

static uint32_t g_w, g_h, g_stride, g_bypp;
static uint32_t tiles_x, tiles_y;
static uint8_t *shadow = NULL;      /* last-sent copy, one full frame */
static int shadow_valid = 0;
static int g_credit = 0;            /* frames we're allowed to send (see FLOW CONTROL) */
static int g_want_key = 0;
#define MAX_CREDIT 2

static void die(const char *msg)
{
    fprintf(stderr, "mx5_screen_server: %s: %s\n", msg, strerror(errno));
    exit(1);
}

/* Refuse to run twice. Two instances both hold /dev/ttyGS1 open and RACE on
 * reads: each input message goes to whichever process reads it first, so a
 * touch DOWN can land in one and the UP in the other -- the first never sees a
 * release and the contact sticks down (a tap becomes a long press), while other
 * taps are split so neither sees a complete one. Their frame writes interleave
 * too. The symptoms look like protocol corruption and are miserable to
 * diagnose, so fail loudly instead.
 *
 * This is easy to do by accident: the installed service has Restart=always, so
 * killing it just brings it back five seconds later, next to the copy you
 * started by hand. Use `systemctl stop mx5-screen` first. */
static void single_instance(void)
{
    static int lock_fd = -1;
    lock_fd = open("/tmp/mx5_screen_server.lock", O_RDWR | O_CREAT, 0600);
    if (lock_fd < 0) return;                       /* can't lock: don't block startup */
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr,
            "mx5_screen_server: another instance is already running.\n"
            "  If it is the installed service:  systemctl stop mx5-screen\n"
            "  Then kill any manual copy:       killall mx5_screen_server\n"
            "  (note: the service restarts itself, so kill alone is not enough)\n");
        exit(1);
    }
}

/* ---------- framebuffer ---------- */

static void fb_open(const char *dev)
{
    fb_fd = open(dev, O_RDONLY);
    if (fb_fd < 0) die("open framebuffer");
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) die("FBIOGET_FSCREENINFO");
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) die("FBIOGET_VSCREENINFO");

    g_w      = vinfo.xres;
    g_h      = vinfo.yres;
    g_stride = finfo.line_length;
    g_bypp   = vinfo.bits_per_pixel / 8;
    if (g_bypp != 4) {
        fprintf(stderr, "only 32bpp supported, got %u\n", vinfo.bits_per_pixel);
        exit(1);
    }
    /* map the whole virtual area: the visible buffer moves with yoffset */
    fbmem_len = finfo.smem_len ? finfo.smem_len : (size_t)g_stride * vinfo.yres_virtual;
    fbmem = mmap(NULL, fbmem_len, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fbmem == MAP_FAILED) die("mmap framebuffer");

    tiles_x = (g_w + TILE - 1) / TILE;
    tiles_y = (g_h + TILE - 1) / TILE;
    shadow = calloc((size_t)g_w * g_h, g_bypp);
    if (!shadow) die("alloc shadow");

    fprintf(stderr, "fb: %ux%u %ubpp stride=%u virtual_h=%u (%s) tiles %ux%u\n",
            g_w, g_h, vinfo.bits_per_pixel, g_stride, vinfo.yres_virtual,
            finfo.id, tiles_x, tiles_y);
}

/* Base of the currently visible buffer. Re-read yoffset every frame: with
 * triple buffering the compositor pans between three buffers, and reading the
 * wrong one yields stale or torn frames. */
static const uint8_t *fb_visible(void)
{
    struct fb_var_screeninfo v;
    uint32_t yoff = 0;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) == 0) yoff = v.yoffset;
    size_t off = (size_t)yoff * g_stride;
    if (off + (size_t)g_h * g_stride > fbmem_len) off = 0;
    return fbmem + off;
}

/* ---------- uinput touch injection ---------- */

/* Separate uinput device for the keyboard.
 *
 * The rig/set-list naming dialog is a real Qt Quick Controls TextField with
 * focus:true and an onAccepted handler -- not a grid of tappable buttons -- so
 * it consumes ordinary Qt key events and a virtual keyboard reaches it the same
 * way our virtual touchscreen reaches the rest of the UI.
 *
 * It is a distinct device rather than more capabilities on the touch device
 * because Qt classifies a device by what it advertises, and a single node
 * claiming both absolute multitouch and a full keymap invites it to be
 * misclassified as one or the other. */
static int g_verbose = 0;
static int kb_fd = -1;
static unsigned char kb_held[KEY_MAX + 1];

/* Which /dev/input/eventN a device lands on decides whether Evil will look at
 * it. inMusic patched Qt's keyboard manager with a hardcoded blacklist --
 * "/dev/input/event2", "/dev/input/event3" and "/dev/input/event4" sit in
 * qevdevkeyboardmanager.cpp's string pool next to a "Not adding keyboard at
 * %ls" message that stock Qt does not have. Presumably those nodes are
 * something they did not want grabbed as a keyboard on the real hardware.
 *
 * Confirmed on device: passing QT_QPA_EVDEV_KEYBOARD_PARAMETERS=/dev/input/event3
 * logs exactly "Not adding keyboard at /dev/input/event3" -- the manager runs,
 * sees our device and refuses it. Touch is unaffected; only keyboards are
 * filtered, which is why our touchscreen works on event2.
 *
 * So place the keyboard outside that range. We cannot choose a node number, but
 * we can burn the bad ones: create a device, ask which node it got, and if it is
 * blacklisted keep it open as padding (so the number stays taken) and try again.
 * Three attempts is the worst case. */
static int kb_blacklisted(int n) { return n >= 2 && n <= 4; }

#define KB_PAD_MAX 8
static int kb_pads[KB_PAD_MAX];
static int kb_npads = 0;

/* Node number the kernel gave a freshly created uinput device, or -1. */
static int uinput_event_number(int fd)
{
    char sys[64], path[160];
    struct dirent *e;
    DIR *dir;
    int n = -1;

    if (ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys) < 0) return -1;
    snprintf(path, sizeof path, "/sys/devices/virtual/input/%s", sys);
    if (!(dir = opendir(path))) return -1;
    while ((e = readdir(dir)))
        if (!strncmp(e->d_name, "event", 5)) { n = atoi(e->d_name + 5); break; }
    closedir(dir);
    return n;
}

static int kb_create(void)
{
    struct uinput_setup us;
    int fd = open("/dev/uinput", O_WRONLY);
    if (fd < 0) { perror("open /dev/uinput (keyboard disabled)"); return -1; }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    /* Advertise the whole ordinary keymap: the viewer decides what to send, so
     * the server needs no per-key policy. Setting every key bit here is also
     * what makes udev tag the node ID_INPUT_KEYBOARD, which Qt's discovery
     * requires. */
    for (int k = KEY_ESC; k <= KEY_COMPOSE; k++)
        ioctl(fd, UI_SET_KEYBIT, k);

    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor  = 0x0763;
    us.id.product = 0x5019;
    snprintf(us.name, sizeof us.name, "mx5-remote-keyboard");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) perror("UI_DEV_SETUP (kbd)");
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE (keyboard disabled)");
        close(fd);
        return -1;
    }
    return fd;
}

static void kb_open(void)
{
    while (kb_npads < KB_PAD_MAX) {
        int fd = kb_create();
        int n;
        if (fd < 0) return;

        n = uinput_event_number(fd);
        if (n < 0 || !kb_blacklisted(n)) {
            kb_fd = fd;
            fprintf(stderr, "uinput: virtual keyboard created (event%d)\n", n);
            return;
        }
        /* Hold it open: closing would free the number and we would get it back. */
        fprintf(stderr, "uinput: event%d is on Evil's keyboard blacklist, retrying\n", n);
        kb_pads[kb_npads++] = fd;
    }
    fprintf(stderr, "uinput: could not get a usable keyboard node (keyboard disabled)\n");
}

static void kb_key(int code, int down)
{
    struct input_event e[2];
    if (kb_fd < 0 || code <= 0 || code > KEY_MAX) return;
    if (g_verbose) fprintf(stderr, "key %d %s\n", code, down ? "down" : "up");

    memset(e, 0, sizeof e);
    e[0].type = EV_KEY; e[0].code = code; e[0].value = down ? 1 : 0;
    e[1].type = EV_SYN; e[1].code = SYN_REPORT;
    if (write(kb_fd, e, sizeof e) != (ssize_t)sizeof e)
        fprintf(stderr, "uinput: DROPPED key %d (%s)\n", code, strerror(errno));
    kb_held[code] = down ? 1 : 0;
}

/* Same hazard as a stuck touch contact, and worse for a modifier: a Shift left
 * down would corrupt every subsequent keystroke from the real UI. */
static void kb_release_all(void)
{
    for (int k = 0; k <= KEY_MAX; k++)
        if (kb_held[k]) kb_key(k, 0);
}

static void ui_open(void)
{
    struct uinput_setup us;
    struct uinput_abs_setup abs;

    /* NOT O_NONBLOCK: with a non-blocking fd a full buffer makes write() fail
     * with EAGAIN, and a silently dropped touch-RELEASE leaves the contact
     * stuck down -- i.e. a tap becomes a long press. uinput writes do not
     * block in practice. */
    ui_fd = open("/dev/uinput", O_WRONLY);
    if (ui_fd < 0) { perror("open /dev/uinput (touch disabled)"); return; }

    ioctl(ui_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(ui_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(ui_fd, UI_SET_EVBIT, EV_SYN);
    ioctl(ui_fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(ui_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    memset(&abs, 0, sizeof abs);
    abs.code = ABS_X;        abs.absinfo.minimum = 0; abs.absinfo.maximum = g_w - 1;
    ioctl(ui_fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_Y;        abs.absinfo.maximum = g_h - 1;
    ioctl(ui_fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_MT_POSITION_X; abs.absinfo.maximum = g_w - 1;
    ioctl(ui_fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_MT_POSITION_Y; abs.absinfo.maximum = g_h - 1;
    ioctl(ui_fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_MT_SLOT;       abs.absinfo.maximum = 1;
    ioctl(ui_fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_MT_TRACKING_ID; abs.absinfo.maximum = 65535;
    ioctl(ui_fd, UI_ABS_SETUP, &abs);

    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor  = 0x0763;
    us.id.product = 0x5018;
    snprintf(us.name, sizeof us.name, "mx5-remote-touch");
    if (ioctl(ui_fd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); }
    if (ioctl(ui_fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE (touch disabled)");
        close(ui_fd); ui_fd = -1; return; }
    fprintf(stderr, "uinput: virtual touchscreen created (%ux%u)\n", g_w, g_h);
}

static void ev(int type, int code, int val)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = val;
    ssize_t w = write(ui_fd, &e, sizeof e);
    if (w != (ssize_t)sizeof e)
        fprintf(stderr, "uinput: DROPPED event type=%d code=%d val=%d (%s)\n",
                type, code, val, strerror(errno));
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int g_touch_down = 0;
static int g_encoder_down = 0;
static void encoder_press(int down);

static void touch(int down, int x, int y)
{
    static int tracking = 1;
    static long last_ms = 0;
    if (ui_fd < 0) return;
    if (g_verbose) {
        long t = now_ms();
        fprintf(stderr, "touch %-5s x=%3d y=%3d  (+%ldms)\n",
                down ? "DOWN" : "UP", x, y, last_ms ? t - last_ms : 0);
        last_ms = t;
    }
    if (x < 0) x = 0;
    if ((uint32_t)x >= g_w) x = (int)g_w - 1;
    if (y < 0) y = 0;
    if ((uint32_t)y >= g_h) y = (int)g_h - 1;

    ev(EV_ABS, ABS_MT_SLOT, 0);
    if (down) {
        ev(EV_ABS, ABS_MT_TRACKING_ID, tracking);
        ev(EV_ABS, ABS_MT_POSITION_X, x);
        ev(EV_ABS, ABS_MT_POSITION_Y, y);
        ev(EV_KEY, BTN_TOUCH, 1);
        ev(EV_ABS, ABS_X, x);
        ev(EV_ABS, ABS_Y, y);
    } else {
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
        ev(EV_KEY, BTN_TOUCH, 0);
        if (++tracking > 60000) tracking = 1;
    }
    ev(EV_SYN, SYN_REPORT, 0);
    g_touch_down = down;
}

/* Never exit holding a contact down. If we die mid-touch, the last thing Qt saw
 * was a press with no release, so whichever QML item took that press keeps its
 * grab -- and a Qt grab is scene-wide, not per-device, so the REAL touchscreen
 * stops responding too. Recovering needs a reboot, which is a nasty thing to
 * leave behind just because someone stopped the service at the wrong moment. */
static void release_touch_on_exit(void)
{
    if (g_touch_down) touch(0, 0, 0);
    if (g_encoder_down) encoder_press(0);
    kb_release_all();
}

static void on_signal(int sig)
{
    release_touch_on_exit();
    _exit(128 + sig);
}


/* ---------- encoder injection via ALSA sequencer ---------- */
/* libasound is loaded at runtime so this stays optional: if it is missing the
 * screen share still works, only the encoder is unavailable. Types are opaque
 * pointers here to avoid a build-time dependency on the ALSA headers. */
static void *g_alsa;
static void *g_seq;                 /* snd_seq_t*  */
static int g_seq_port = -1;         /* our source port */
static int g_dst_client = -1, g_dst_port = -1;

static int (*p_open)(void **, const char *, int, int);
static int (*p_set_name)(void *, const char *);
static int (*p_create_simple_port)(void *, const char *, unsigned, unsigned);
static int (*p_connect_to)(void *, int, int, int);
static int (*p_event_output_direct)(void *, void *);
static int (*p_drain_output)(void *);
static int (*p_query_next_client)(void *, void *);
static int (*p_query_next_port)(void *, void *);
static int (*p_client_info_malloc)(void **);
static int (*p_port_info_malloc)(void **);
static void (*p_client_info_set_client)(void *, int);
static void (*p_port_info_set_client)(void *, int);
static void (*p_port_info_set_port)(void *, int);
static int (*p_client_info_get_client)(void *);
static int (*p_port_info_get_port)(void *);
static const char *(*p_client_info_get_name)(void *);

#define SND_SEQ_OPEN_OUTPUT 1
#define SND_SEQ_PORT_CAP_READ 0x01
#define SND_SEQ_PORT_CAP_SUBS_READ 0x20
#define SND_SEQ_PORT_TYPE_MIDI_GENERIC 0x00000002
#define SND_SEQ_PORT_TYPE_APPLICATION  0x00100000

/* Minimal snd_seq_event_t layout (ALSA ABI, stable). We only fill the fields a
 * direct-delivery MIDI event needs. */
struct seq_addr { unsigned char client, port; };
struct seq_ev_ctrl { unsigned char channel, unused[3]; unsigned int param; signed int value; };
struct seq_ev_note { unsigned char channel, note, velocity, off_velocity; unsigned int duration; };
struct seq_event {
    unsigned char type, flags, tag, queue;
    unsigned int tick_or_time[2];
    struct seq_addr source, dest;
    union { struct seq_ev_note note; struct seq_ev_ctrl ctrl; unsigned char raw[32]; } data;
};
#define SND_SEQ_EVENT_NOTEON      6
#define SND_SEQ_EVENT_NOTEOFF     7
#define SND_SEQ_EVENT_CONTROLLER  10
#define SND_SEQ_TIME_STAMP_REAL   1
#define SND_SEQ_TIME_MODE_REL     2
#define SND_SEQ_QUEUE_DIRECT      253
#define SND_SEQ_ADDRESS_SUBSCRIBERS 254

static void *dl(const char *sym) { return dlsym(g_alsa, sym); }

/* Find Evil's input port for the control surface by NAME (client numbers are
 * assigned dynamically, so hardcoding 129 would be fragile). */
static int find_evil_port(const char *want)
{
    void *cinfo = NULL, *pinfo = NULL;
    if (p_client_info_malloc(&cinfo) < 0 || p_port_info_malloc(&pinfo) < 0) return -1;
    p_client_info_set_client(cinfo, -1);
    while (p_query_next_client(g_seq, cinfo) >= 0) {
        int c = p_client_info_get_client(cinfo);
        const char *name = p_client_info_get_name(cinfo);
        if (!name || !strstr(name, want)) continue;
        p_port_info_set_client(pinfo, c);
        p_port_info_set_port(pinfo, -1);
        if (p_query_next_port(g_seq, pinfo) >= 0) {
            g_dst_client = c;
            g_dst_port = p_port_info_get_port(pinfo);
            fprintf(stderr, "encoder: found \"%s\" at %d:%d\n", name, g_dst_client, g_dst_port);
            return 0;
        }
    }
    return -1;
}

static void encoder_open(void)
{
    g_alsa = dlopen("libasound.so.2", RTLD_NOW);
    if (!g_alsa) { fprintf(stderr, "encoder: libasound not available (%s) -- disabled\n", dlerror()); return; }
    p_open                  = dl("snd_seq_open");
    p_set_name              = dl("snd_seq_set_client_name");
    p_create_simple_port    = dl("snd_seq_create_simple_port");
    p_connect_to            = dl("snd_seq_connect_to");
    p_event_output_direct   = dl("snd_seq_event_output_direct");
    p_drain_output          = dl("snd_seq_drain_output");
    p_query_next_client     = dl("snd_seq_query_next_client");
    p_query_next_port       = dl("snd_seq_query_next_port");
    p_client_info_malloc    = dl("snd_seq_client_info_malloc");
    p_port_info_malloc      = dl("snd_seq_port_info_malloc");
    p_client_info_set_client= dl("snd_seq_client_info_set_client");
    p_port_info_set_client  = dl("snd_seq_port_info_set_client");
    p_port_info_set_port    = dl("snd_seq_port_info_set_port");
    p_client_info_get_client= dl("snd_seq_client_info_get_client");
    p_port_info_get_port    = dl("snd_seq_port_info_get_port");
    p_client_info_get_name  = dl("snd_seq_client_info_get_name");
    if (!p_open || !p_create_simple_port || !p_event_output_direct || !p_query_next_client) {
        fprintf(stderr, "encoder: libasound missing expected symbols -- disabled\n");
        g_alsa = NULL; return;
    }
    if (p_open(&g_seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        fprintf(stderr, "encoder: snd_seq_open failed -- disabled\n"); g_seq = NULL; return;
    }
    p_set_name(g_seq, "mx5-remote-encoder");
    g_seq_port = p_create_simple_port(g_seq, "out",
                    SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                    SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (g_seq_port < 0) { fprintf(stderr, "encoder: create_simple_port failed -- disabled\n"); g_seq = NULL; return; }
    if (find_evil_port("Midi::In::HG04 Control Surface MIDI 1") < 0) {
        fprintf(stderr, "encoder: Evil's control-surface input port not found -- disabled\n");
        g_seq = NULL; return;
    }
    if (p_connect_to(g_seq, g_seq_port, g_dst_client, g_dst_port) < 0)
        fprintf(stderr, "encoder: connect_to %d:%d failed (will send addressed instead)\n",
                g_dst_client, g_dst_port);
    fprintf(stderr, "encoder: ready\n");
}

static void seq_send(struct seq_event *e)
{
    if (!g_seq) return;
    e->queue = SND_SEQ_QUEUE_DIRECT;
    e->flags = SND_SEQ_TIME_STAMP_REAL | SND_SEQ_TIME_MODE_REL;
    e->source.client = 0; e->source.port = (unsigned char)g_seq_port;
    e->dest.client = (unsigned char)g_dst_client;
    e->dest.port = (unsigned char)g_dst_port;
    p_event_output_direct(g_seq, e);
    if (p_drain_output) p_drain_output(g_seq);
}

/* Encoder turn. The stock assignment feeds CC 3 into a JogOutput, which expects
 * a RELATIVE value -- but airAssignments' exact convention isn't documented in
 * the firmware, and the three common ones disagree on how negative is encoded.
 * Selectable with -j so it can be settled empirically without a rebuild:
 *   0 two's complement:            +1 -> 1,  -1 -> 127
 *   1 signed bit (DEFAULT):        +1 -> 1,  -1 -> 65   <- verified on hardware
 *   2 binary offset (64 = zero):   +1 -> 65, -1 -> 63
 * Mode 1 is what the MX5's JogOutput actually expects (confirmed: direction is
 * correct in both directions). The others are kept for other devices/firmware. */
static int g_jog_mode = 1;

static void encoder_turn(int delta)
{
    if (g_verbose) fprintf(stderr, "encoder turn %+d\n", delta);
    int steps = delta < 0 ? -delta : delta;
    int neg = delta < 0;
    if (steps > 16) steps = 16;
    for (int i = 0; i < steps; i++) {
        int v;
        switch (g_jog_mode) {
            case 0:  v = neg ? 127 : 1; break;   /* two's complement */
            case 2:  v = neg ? 63 : 65; break;    /* binary offset */
            default: v = neg ? 65 : 1;  break;    /* signed bit (MX5) */
        }
        struct seq_event e; memset(&e, 0, sizeof e);
        e.type = SND_SEQ_EVENT_CONTROLLER;
        e.data.ctrl.channel = 0;
        e.data.ctrl.param = 3;                       /* CC 3 */
        e.data.ctrl.value = v;
        seq_send(&e);
    }
}

/* Encoder push: note 4 on/off, which the stock assignment routes through
 * PressAndHoldOutput to EncoderEnter (+ EncoderTimer for long press). */
static void encoder_press(int down)
{
    g_encoder_down = down;
    if (g_verbose) fprintf(stderr, "encoder %s\n", down ? "PRESS" : "RELEASE");
    struct seq_event e; memset(&e, 0, sizeof e);
    e.type = down ? SND_SEQ_EVENT_NOTEON : SND_SEQ_EVENT_NOTEOFF;
    e.data.note.channel = 0;
    e.data.note.note = 4;
    e.data.note.velocity = down ? 127 : 0;
    seq_send(&e);
}

/* ---------- output ---------- */

static int write_all(const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(out_fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= w;
    }
    return 0;
}

#define HDR_LEN 24

/* Compare against the shadow and emit changed tiles. Returns tiles sent. */
static uint32_t send_frame(const uint8_t *src, int force)
{
    static uint8_t *payload = NULL;
    static size_t payload_cap = 0;
    size_t tile_bytes = (size_t)TILE * TILE * g_bypp;
    /* 9 bytes of per-tile header, and the payload never exceeds raw size */
    size_t need = HDR_LEN + (size_t)tiles_x * tiles_y * (9 + tile_bytes);
    if (payload_cap < need) {
        payload = realloc(payload, need);
        if (!payload) die("alloc payload");
        payload_cap = need;
    }

    uint8_t *o = payload + HDR_LEN;     /* leave room for the frame header */
    uint32_t ntiles = 0;

    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        uint32_t y0 = ty * TILE, yh = (y0 + TILE <= g_h) ? TILE : g_h - y0;
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint32_t x0 = tx * TILE, xw = (x0 + TILE <= g_w) ? TILE : g_w - x0;
            size_t row_bytes = (size_t)xw * g_bypp;
            int changed = force || !shadow_valid;

            if (!changed) {
                for (uint32_t r = 0; r < yh; r++) {
                    const uint8_t *s = src + (size_t)(y0 + r) * g_stride + (size_t)x0 * g_bypp;
                    const uint8_t *d = shadow + ((size_t)(y0 + r) * g_w + x0) * g_bypp;
                    if (memcmp(s, d, row_bytes)) { changed = 1; break; }
                }
            }
            if (!changed) continue;

            /* Gather the tile contiguously (zero-padded at the edges so the
             * viewer's blit is uniform) and refresh the shadow. */
            static uint8_t tmp[TILE * TILE * 4];
            memset(tmp, 0, tile_bytes);
            for (uint32_t r = 0; r < yh; r++) {
                const uint8_t *s = src + (size_t)(y0 + r) * g_stride + (size_t)x0 * g_bypp;
                memcpy(tmp + (size_t)r * TILE * g_bypp, s, row_bytes);
                uint8_t *d = shadow + ((size_t)(y0 + r) * g_w + x0) * g_bypp;
                memcpy(d, s, row_bytes);
            }

            /* Tile records are variable length (RLE payloads are multiples of
             * 6), so `o` is not aligned -- use memcpy rather than unaligned
             * pointer stores. */
            uint16_t t16;
            t16 = (uint16_t)tx; memcpy(o, &t16, 2);
            t16 = (uint16_t)ty; memcpy(o + 2, &t16, 2);
            uint8_t *enc_at = o + 4;
            uint8_t *len_at = o + 5;
            uint8_t *dst = o + 9;

            /* RLE over 32-bit pixels; bail out to raw as soon as it would
             * exceed the raw size, so we never make a tile bigger. */
            const uint32_t *px = (const uint32_t *)tmp;
            uint32_t npx = TILE * TILE, w_out = 0;
            int use_rle = 1;
            for (uint32_t i = 0; i < npx; ) {
                uint32_t v = px[i], run = 1;
                while (i + run < npx && px[i + run] == v && run < 0xFFFF) run++;
                if (w_out + 6 > tile_bytes) { use_rle = 0; break; }
                uint16_t r16 = (uint16_t)run;
                memcpy(dst + w_out, &r16, 2);
                memcpy(dst + w_out + 2, &v, 4);
                w_out += 6;
                i += run;
            }
            if (!use_rle) {
                memcpy(dst, tmp, tile_bytes);
                *enc_at = ENC_RAW;
                uint32_t l32 = (uint32_t)tile_bytes; memcpy(len_at, &l32, 4);
                o = dst + tile_bytes;
            } else {
                *enc_at = ENC_RLE;
                memcpy(len_at, &w_out, 4);
                o = dst + w_out;
            }
            ntiles++;
        }
    }

    /* Self-describing header on every frame -- lets a viewer attach or resync
     * at any point by scanning for the magic. */
    memcpy(payload, "MX5S", 4);
    *(uint16_t *)(payload + 4)  = PROTO_VER;
    *(uint16_t *)(payload + 6)  = TILE;
    *(uint32_t *)(payload + 8)  = g_w;
    *(uint32_t *)(payload + 12) = g_h;
    *(uint32_t *)(payload + 16) = g_bypp * 8;
    *(uint32_t *)(payload + 20) = ntiles;
    if (write_all(payload, (size_t)(o - payload)) < 0) die("write frame");
    shadow_valid = 1;
    return ntiles;
}

/* ---------- input from host ---------- */

static void handle_input(void)
{
    static uint8_t buf[1024];
    static size_t have = 0;

    /* Drain everything currently available, not just one small chunk.
     * This is called once per frame tick (~15/s); a client sends a touch event
     * per mousemove while dragging, plus an ack per frame, plus key repeat --
     * far more than one 64-byte read per tick can absorb. Under-reading makes
     * input back up, which shows as clicks that do nothing and taps that behave
     * like long presses, because the touch-RELEASE arrives seconds late. */
    for (;;) {
        if (have >= sizeof buf) break;
        struct pollfd pfd = { .fd = 0, .events = POLLIN };
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
        ssize_t n = read(0, buf + have, sizeof buf - have);
        if (n <= 0) break;
        have += n;
    }
    if (have == 0) return;

    size_t i = 0;
    while (i < have) {
        if (buf[i] == 'K') { g_want_key = 1; g_credit = MAX_CREDIT; i++; continue; }
        if (buf[i] == 'A') { if (g_credit < MAX_CREDIT) g_credit++; i++; continue; }
        if (buf[i] == 'E') {
            if (have - i < 2) break;
            encoder_turn((signed char)buf[i + 1]);
            i += 2;
            continue;
        }
        if (buf[i] == 'P') {
            if (have - i < 2) break;
            encoder_press(buf[i + 1]);
            i += 2;
            continue;
        }
        if (buf[i] == 'T') {
            if (have - i < 6) break;            /* wait for the rest */
            int down = buf[i + 1];
            int x = buf[i + 2] | (buf[i + 3] << 8);
            int y = buf[i + 4] | (buf[i + 5] << 8);
            touch(down, x, y);
            i += 6;
            continue;
        }
        if (buf[i] == 'B') {                    /* keyboard: code + down */
            if (have - i < 4) break;
            int code = buf[i + 1] | (buf[i + 2] << 8);
            kb_key(code, buf[i + 3]);
            i += 4;
            continue;
        }
        i++;                                    /* skip noise */
    }
    if (i && i < have) memmove(buf, buf + i, have - i);
    have -= i;
}

int main(int argc, char **argv)
{
    const char *fbdev = "/dev/fb0";
    int fps = 15;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) fbdev = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-j") && i + 1 < argc) g_jog_mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else {
            fprintf(stderr,
                "usage: %s [-f /dev/fb0] [-r fps]\n"
                "  stdin/stdout are the transport; run as:\n"
                "    taskset -c 3 %s <> /dev/ttyGS1 >&0\n", argv[0], argv[0]);
            return 2;
        }
    }
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
    out_fd = 1;

    /* If stdout is a tty (the gadget serial port), put it in raw mode so our
     * binary stream isn't mangled by line discipline processing. */
    if (isatty(out_fd)) {
        struct termios t;
        if (tcgetattr(out_fd, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(out_fd, TCSANOW, &t);
        }
    }

    single_instance();
    fb_open(fbdev);
    ui_open();
    kb_open();
    atexit(release_touch_on_exit);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGPIPE, SIG_IGN);
    encoder_open();

    long period_ns = 1000000000L / fps;
    struct timespec next, last_key;
    clock_gettime(CLOCK_MONOTONIC, &next);
    last_key = next;
    int force = 1;                      /* first frame is always full */

    for (;;) {
        struct pollfd p = { .fd = 0, .events = POLLIN };
        /* Block here when we have no credit: nothing to do until the client
         * acks or asks for a keyframe, and spinning would just burn a core. */
        if (poll(&p, 1, g_credit > 0 ? 0 : 200) > 0 && (p.revents & POLLIN))
            handle_input();
        if (g_want_key) { force = 1; g_want_key = 0; }

        /* Periodic keyframe: safety net for a viewer that attached without its
         * 'K' request being seen (e.g. while we were blocked writing). */
        if (next.tv_sec - last_key.tv_sec >= KEYFRAME_SEC) { force = 1; last_key = next; }

        if (g_credit > 0) {
            send_frame(fb_visible(), force);
            force = 0;
            g_credit--;
        }

        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }

        /* If we fell behind (a big frame, or blocked writing because the host
         * stopped reading), do NOT try to catch up: an already-past absolute
         * deadline makes clock_nanosleep return immediately, and we'd burst
         * frames at full speed -- which is exactly what floods the host's
         * buffer and truncates frames. Just resync the schedule to now. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec ||
            (now.tv_sec == next.tv_sec && now.tv_nsec > next.tv_nsec)) {
            next = now;
            continue;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return 0;
}
