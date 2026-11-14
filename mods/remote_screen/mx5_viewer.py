#!/usr/bin/env python3
"""
mx5_viewer.py -- host-side viewer for mx5_screen_server.

Shows the HeadRush MX5's screen in a window and sends mouse clicks back as
touch events. Talks to the device over the second USB CDC-ACM channel.

    # Windows
    python mx5_viewer.py COM7
    # Linux / macOS
    python mx5_viewer.py /dev/ttyACM1

Requires: pyserial, pygame   (pip install pyserial pygame)

Notes
-----
* The MX5's framebuffer is portrait 480x800 while the UI is landscape 800x480 --
  the panel is mounted rotated. This viewer rotates for display and rotates
  mouse coordinates back before sending them, so clicking what you see works.
* Serial read sizing is the single most important detail here, and it cuts both
  ways -- both mistakes were made and measured on real hardware:
    - The host's *driver* buffer should be large (set_buffer_size below).
      Leaving it at the 4 KB default throttled the link from ~12 MB/s to
      ~0.6 MB/s.
    - But never ask read() for more bytes than you actually need. pyserial's
      read(n) blocks until n bytes arrive OR the timeout expires, so
      "read(65536) for batching" makes every read timeout-bound instead of
      data-bound. That alone produced multi-second lag, and the resulting
      mid-frame timeouts caused stream desyncs. See Reader._fill.
* The server sends only tiles that changed, so an idle screen costs no
  bandwidth. Full frames are sent on request ('K') and as a periodic safety net.
"""
import struct
import sys

try:
    import serial
except ImportError:
    sys.exit("need pyserial:  pip install pyserial")
try:
    import pygame
except ImportError:
    sys.exit("need pygame:  pip install pygame")

TOUCH_DOWN, TOUCH_UP = 1, 0
ENC_RAW, ENC_RLE = 0, 1


MAGIC = b"MX5S"
HDR_LEN = 24
CHUNK = 65536


class Reader:
    """Buffered reader over the serial port.

    Buffering matters for two reasons: throughput (the link does ~12 MB/s, but
    only if we read in big chunks -- byte-at-a-time reads collapse it), and
    correctness (resyncing needs to rescan bytes we've already looked at, which
    is impossible if they've been consumed from the port).
    """

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def _fill(self, n):
        while len(self.buf) < n:
            need = n - len(self.buf)
            # Ask for EXACTLY what's missing. pyserial's read(n) blocks until it
            # has n bytes or the timeout expires, so over-asking (e.g. always
            # requesting 64K "for batching") makes every read timeout-bound
            # instead of data-bound -- which stalls a frame for the full timeout
            # and was the real cause of multi-second lag.
            chunk = self.ser.read(need)
            if not chunk:
                raise IOError("serial timeout / device stopped sending")
            self.buf += chunk
            # Then drain whatever else already arrived. This asks only for bytes
            # the driver is holding, so it returns immediately -- batching
            # without ever waiting.
            extra = self.ser.in_waiting
            if extra:
                self.buf += self.ser.read(min(extra, CHUNK))

    def read_exact(self, n):
        self._fill(n)
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def sync_header(self):
        """Find the next valid frame header and consume it.

        The server streams continuously with no connection semantics, so we may
        attach mid-frame. Pixel data can contain the magic bytes, so fields are
        sanity-checked; on a false match we advance exactly ONE byte and keep
        scanning, rather than skipping the whole 24 bytes (which could swallow a
        real header that started inside the false one).
        """
        while True:
            self._fill(HDR_LEN)
            i = self.buf.find(MAGIC)
            if i < 0:
                # no magic anywhere; keep a 3-byte tail in case it straddles
                del self.buf[:max(0, len(self.buf) - 3)]
                self._fill(len(self.buf) + 1)
                continue
            self._fill(i + HDR_LEN)
            hdr = bytes(self.buf[i:i + HDR_LEN])
            ver, tile = struct.unpack_from("<HH", hdr, 4)
            w, h, bpp, ntiles = struct.unpack_from("<IIII", hdr, 8)
            if (ver == 3 and tile in (8, 16, 32, 64) and bpp == 32
                    and 0 < w <= 4096 and 0 < h <= 4096
                    and ntiles <= ((w + tile - 1) // tile) * ((h + tile - 1) // tile)):
                del self.buf[:i + HDR_LEN]
                return tile, w, h, bpp, ntiles
            del self.buf[:i + 1]        # false positive: advance one byte


def main():
    if len(sys.argv) < 2:
        sys.exit(f"usage: {sys.argv[0]} <serial port>   e.g. COM7 or /dev/ttyACM1")
    port = sys.argv[1]

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200          # ignored by CDC-ACM, but pyserial wants one
    # Short timeout: the viewer blocks in the serial read, so a long timeout
    # freezes the UI (no click handling) for its whole duration on any stall.
    ser.timeout = 1.0
    # Big buffers matter -- see module docstring.
    try:
        ser.set_buffer_size(rx_size=4 * 1024 * 1024, tx_size=64 * 1024)  # Windows only
    except AttributeError:
        pass
    ser.open()

    # We're joining a stream that's already running: drop whatever is buffered,
    # ask for a full frame, then hunt for a header.
    ser.reset_input_buffer()
    ser.write(b"K")

    rd = Reader(ser)
    print("syncing...")
    for attempt in range(5):
        try:
            tile, w, h, bpp, ntiles = rd.sync_header()
            break
        except IOError:
            print(f"  no data yet, re-requesting keyframe ({attempt + 1}/5)")
            ser.write(b"K")
    else:
        sys.exit("no frames from device -- is mx5_screen_server running on /dev/ttyGS1?")
    print(f"connected: {w}x{h} {bpp}bpp, tile={tile}")

    tile_bytes = tile * tile * 4
    first = (ntiles,)          # tile count of the frame we just synced onto

    pygame.init()
    # framebuffer is portrait; display rotated to landscape
    screen = pygame.display.set_mode((h, w))
    pygame.display.set_caption(f"HeadRush MX5 ({port})")
    fb = pygame.Surface((w, h))          # unrotated backing store
    clock = pygame.time.Clock()
    dragging = False
    frames = 0
    resyncs = 0
    stalls = 0

    def send_encoder(delta):
        # The physical knob is the control surface sending MIDI; the server
        # injects the same MIDI into Evil's ALSA sequencer port, so this behaves
        # exactly like turning the real knob.
        try:
            ser.write(struct.pack("<cb", b"E", delta))
        except Exception as e:
            print("encoder write failed:", e)

    def send_encoder_press(down):
        try:
            ser.write(struct.pack("<cB", b"P", 1 if down else 0))
        except Exception as e:
            print("encoder write failed:", e)

    last_xy = [-1, -1]

    def send_touch(down, mx, my):
        # The panel is mounted rotated: framebuffer is portrait (w=480, h=800),
        # the physical screen is landscape (800x480). Display uses
        # pygame.transform.rotate(fb, 90), i.e. counter-clockwise, which maps
        # fb (x,y) -> display (y, w-1-x). This is the exact inverse; getting it
        # wrong (as an earlier version did) lands clicks 180 degrees out.
        x = w - 1 - my
        y = mx
        x = max(0, min(w - 1, int(x)))
        y = max(0, min(h - 1, int(y)))
        # Drop no-op drag updates: MOUSEMOTION fires far faster than the device
        # needs, and flooding the link delays the touch-RELEASE, which turns a
        # tap into a long press. Press and release are always sent.
        if down and [x, y] == last_xy:
            return
        last_xy[:] = [x, y] if down else [-1, -1]
        try:
            ser.write(struct.pack("<cBHH", b"T", down, x, y))
        except Exception as e:
            print("touch write failed:", e)

    running = True
    while running:
        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                running = False
            elif e.type == pygame.MOUSEBUTTONDOWN and e.button == 1:
                dragging = True
                send_touch(TOUCH_DOWN, *e.pos)
            elif e.type == pygame.MOUSEMOTION and dragging:
                send_touch(TOUCH_DOWN, *e.pos)
            elif e.type == pygame.MOUSEBUTTONUP and e.button == 1:
                dragging = False
                send_touch(TOUCH_UP, *e.pos)
            # Encoder: arrows turn it, space/enter presses it.
            elif e.type == pygame.KEYDOWN:
                if e.key in (pygame.K_UP, pygame.K_LEFT):
                    send_encoder(-1)
                elif e.key in (pygame.K_DOWN, pygame.K_RIGHT):
                    send_encoder(1)
                elif e.key in (pygame.K_SPACE, pygame.K_RETURN, pygame.K_KP_ENTER):
                    send_encoder_press(True)
            elif e.type == pygame.KEYUP:
                if e.key in (pygame.K_SPACE, pygame.K_RETURN, pygame.K_KP_ENTER):
                    send_encoder_press(False)

        # ---- one frame ----
        # Any malformed data means we've lost sync (dropped bytes, or we
        # attached mid-frame). Recover by resyncing and asking for a fresh
        # keyframe rather than dying -- an unhandled exception here was what
        # kept killing the viewer.
        try:
            if first:
                ntiles, = first
                first = None               # use the header we synced onto
            else:
                _t, _w2, _h2, _b, ntiles = rd.sync_header()

            for _ in range(ntiles):
                tx, ty, enc, ln = struct.unpack("<HHBI", rd.read_exact(9))
                payload = rd.read_exact(ln)
                if enc == ENC_RLE:
                    out = bytearray()
                    for off in range(0, ln - 5, 6):
                        count = payload[off] | (payload[off + 1] << 8)
                        out += payload[off + 2:off + 6] * count
                    if len(out) != tile_bytes:
                        raise ValueError(f"RLE tile decoded to {len(out)}, want {tile_bytes}")
                    data = bytes(out)
                else:
                    if ln != tile_bytes:
                        raise ValueError(f"raw tile len {ln}, want {tile_bytes}")
                    data = payload
                # The framebuffer is BGR*X* -- the 4th byte is padding, and on
                # this device it is 0x00. Interpreting it as BGRA gives every
                # pixel alpha=0, so blits draw nothing and the window stays
                # black. .convert() drops the per-pixel alpha (unlike
                # .convert_alpha()), leaving an opaque display-format surface.
                surf = pygame.image.frombuffer(data, (tile, tile), "BGRA").convert()
                fb.blit(surf, (tx * tile, ty * tile))
        except (ValueError, struct.error) as e:
            resyncs += 1
            print(f"desync ({e}) -- resyncing [{resyncs}]")
            rd.buf.clear()      # stale mid-frame bytes; scanning them for the
                                # magic is what yields false-positive headers
            ser.reset_input_buffer()
            ser.write(b"K")
            first = None
            continue
        except IOError:
            # No data within the timeout. Usually just means the server has no
            # credit / nothing changed; nudge it and keep the UI responsive
            # rather than blocking for a long stretch.
            stalls += 1
            if stalls % 10 == 1:
                print(f"link idle [{stalls}] -- nudging")
            rd.buf.clear()
            ser.write(b"K")
            first = None
            continue

        # Ack: the server only sends the next frame once we've consumed this
        # one, which bounds latency (see FLOW CONTROL in the server).
        ser.write(b"A")

        frames += 1
        if frames <= 3 or frames % 100 == 0:
            print(f"frame {frames}: {ntiles} tiles")

        # Only re-render when something actually changed. Rotating a 480x800
        # surface costs real CPU, and doing it on every heartbeat frame was
        # enough to stop the host draining the port fast enough -- which
        # overflowed the buffer and truncated large frames.
        if ntiles:
            rotated = pygame.transform.rotate(fb, 90)
            screen.blit(rotated, (0, 0))
            pygame.display.flip()
        clock.tick(120)

    ser.close()
    pygame.quit()


if __name__ == "__main__":
    main()
