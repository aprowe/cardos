/* Screen -- the PC's desktop, live, on the Cardputer.
 *
 * server/screen.py captures the machine that renders web pages and encodes it
 * as SCR1: a stream of rows rather than frames.
 *
 *     "SCR1" u16 w u16 h        once
 *     then forever:
 *       u16 y                   the row, or 0xFFFF for end of frame
 *       u16 n                   bytes of RLE
 *       n bytes                 RLE, the codec .cpx already uses
 *
 * Only rows that changed are sent, so a still desktop costs almost nothing --
 * measured at 8 KB/s, against 104 KB/s when every row changes every frame.
 *
 * The consequence for this app is that **there is no framebuffer**. A row
 * arrives, is decoded into 480 bytes, is blitted, and is forgotten. A row that
 * does not arrive is a row that does not change, so nothing needs remembering
 * between frames. That is the damage channel's trick applied to time instead
 * of space, and it is what lets a machine with 120 KB of heap show live video.
 *
 * Two views, and the default is the interesting one. Scaling a 1920x1080
 * desktop into 240x135 gives unreadable mush -- the lesson the web renderer
 * already taught. So `follow` sends a 240x135 window of the real screen at one
 * pixel per pixel, tracking the mouse: text is sharp because nothing is
 * resampled, and what you are pointing at is what is on the device. `fit`
 * scales the whole screen for orientation and is deliberately unreadable.
 *
 * While the stream runs the shell is blocked -- see CardApi.http_stream. That
 * is why this app is fullscreen only and why any key stops it: there is
 * nothing else running to notice one.
 */

#include "kernel/app/capp.h"

#define W        240
#define H        135
#define ROWBUF   700          /* a row is at most 240*2 plus its run bytes */
#define URLMAX   160

#define CLR_BG   CAPP_RGB(8, 10, 14)
#define CLR_FG   CAPP_RGB(226, 232, 242)
#define CLR_DIM  CAPP_RGB(130, 140, 158)
#define CLR_BAR  CAPP_RGB(32, 48, 78)

static const CardApi *api;

/* The decoder is a state machine because a row can straddle two chunks off
 * the socket, and the socket knows nothing about rows. */
enum { WANT_MAGIC = 0, WANT_ROWHDR, WANT_PAYLOAD };

static struct {
  char base[96];
  char mode[8];              /* "follow" or "fit" */

  int  state;
  int  need;                 /* bytes still wanted for the current piece */
  int  have;
  uint8_t hdr[8];
  uint8_t raw[ROWBUF];
  uint16_t row[W];
  int  y;                    /* the row being received */

  uint32_t frames;
  uint32_t bytes;
  uint32_t started;
  int  stop;                 /* a key was seen: unwind out of the stream */
  int  running;
  char status[64];
} S;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---- decoding -------------------------------------------------------------- */

static int decode_row(int len) {
  int i = 0, out = 0;

  while (i < len && out < W) {
    uint8_t c = S.raw[i++];
    if (c < 128) {
      int n = c + 1;
      uint16_t px;
      if (i + 2 > len) return 0;
      px = rd16(S.raw + i);
      i += 2;
      while (n-- > 0 && out < W) S.row[out++] = px;
    } else {
      int n = c - 127;
      if (i + 2 * n > len) return 0;
      while (n-- > 0 && out < W) { S.row[out++] = rd16(S.raw + i); i += 2; }
    }
  }
  while (out < W) S.row[out++] = 0;
  return 1;
}

/* One chunk off the socket, fed through the state machine. Returns non-zero to
 * stop the stream, which is how escape gets out of a transfer that would
 * otherwise never end. */
static int on_data(void *ctx, const uint8_t *d, int n) {
  int i = 0;
  (void)ctx;

  S.bytes += (uint32_t)n;

  while (i < n) {
    switch (S.state) {
    case WANT_MAGIC: {
      int take = 8 - S.have;
      if (take > n - i) take = n - i;
      api->mem_cpy(S.hdr + S.have, d + i, (size_t)take);
      S.have += take;
      i += take;
      if (S.have < 8) return 0;
      /* "SCR1" then the size, which had better be the panel's. */
      if (S.hdr[0] != 'S' || S.hdr[1] != 'C' || S.hdr[2] != 'R' || S.hdr[3] != '1')
        return 1;
      S.state = WANT_ROWHDR;
      S.have = 0;
      break;
    }

    case WANT_ROWHDR: {
      int take = 4 - S.have;
      if (take > n - i) take = n - i;
      api->mem_cpy(S.hdr + S.have, d + i, (size_t)take);
      S.have += take;
      i += take;
      if (S.have < 4) return 0;
      S.y = rd16(S.hdr);
      S.need = rd16(S.hdr + 2);
      S.have = 0;
      if (S.y == 0xFFFF) {
        /* End of frame. The only moment worth asking whether to stop: once a
         * frame, rather than once a chunk, so a keypress ends a whole picture
         * rather than half of one. */
        S.frames++;
        S.state = WANT_ROWHDR;
        if (api->key_pending()) { S.stop = 1; return 1; }
        break;
      }
      if (S.need <= 0 || S.need > ROWBUF) return 1;   /* malformed: give up */
      S.state = WANT_PAYLOAD;
      break;
    }

    case WANT_PAYLOAD: {
      int take = S.need - S.have;
      if (take > n - i) take = n - i;
      api->mem_cpy(S.raw + S.have, d + i, (size_t)take);
      S.have += take;
      i += take;
      if (S.have < S.need) return 0;

      if (decode_row(S.need) && S.y < H)
        api->pixels(rect(0, S.y, W, 1), S.row);

      S.have = 0;
      S.state = WANT_ROWHDR;
      break;
    }

    default:
      return 1;
    }
  }
  return 0;
}

/* ---- painting --------------------------------------------------------------- */

/* Only ever drawn when the stream is *not* running: while it is, the rows are
 * the picture and there is nothing else to say. */
static void app_paint(void *st, CRect c) {
  (void)st;
  api->fill(c, CLR_BG);
  api->text((short)(c.x + 8), (short)(c.y + 30), "Screen", CLR_FG, CLR_BG);
  api->text((short)(c.x + 8), (short)(c.y + 46), S.status, CLR_DIM, CLR_BG);
  api->text((short)(c.x + 8), (short)(c.y + 62), "enter\tconnect", CLR_DIM, CLR_BG);
  api->text((short)(c.x + 8), (short)(c.y + 74), "f\tfollow the mouse / whole screen",
            CLR_DIM, CLR_BG);
  api->fill(rect(c.x, c.y + c.h - 9, c.w, 9), CLR_BAR);
  {
    char bar[64];
    api->fmt(bar, sizeof bar, "%s  %s", S.mode, S.base);
    api->text((short)(c.x + 2), (short)(c.y + c.h - 8), bar, CLR_FG, CLR_BAR);
  }
}

/* ---- running ---------------------------------------------------------------- */

static void connect(void) {
  char url[URLMAX];
  int n;

  if (!api->net_ready()) {
    api->fmt(S.status, sizeof S.status, "%s", "connecting to wifi...");
    if (api->net_connect(20000) != 0) {
      api->fmt(S.status, sizeof S.status, "%s", api->net_status());
      return;
    }
  }

  api->fmt(url, sizeof url, "%s/screen?mode=%s&fps=12", S.base, S.mode);
  api->mem_set(&S.state, 0, sizeof S.state);
  S.state = WANT_MAGIC;
  S.have = 0;
  S.frames = 0;
  S.bytes = 0;
  S.stop = 0;
  S.running = 1;
  S.started = api->ticks_ms();

  /* This does not return until a key is pressed or the server stops. The
   * whole machine is inside it: no clock, no other app, nothing. That is the
   * bargain a viewer makes, and it is why any key ends it. */
  n = api->http_stream(url, on_data, (void *)0, 60000);
  S.running = 0;

  {
    uint32_t secs = (api->ticks_ms() - S.started) / 1000u;
    if (n < 0)
      api->fmt(S.status, sizeof S.status, "stream failed (%d)", n);
    else if (secs)
      api->fmt(S.status, sizeof S.status, "%lu frames, %lu KB, %lu fps",
               (unsigned long)S.frames, (unsigned long)(S.bytes / 1024),
               (unsigned long)(S.frames / secs));
    else
      api->fmt(S.status, sizeof S.status, "%lu frames", (unsigned long)S.frames);
  }
}

static int app_key(void *st, unsigned char k) {
  (void)st;

  switch (k) {
  case CAPP_KEY_ENTER:
  case ' ':
    connect();
    return 1;
  case 'f': case 'F':
    api->fmt(S.mode, sizeof S.mode, "%s",
             S.mode[0] == 'f' && S.mode[1] == 'o' ? "fit" : "follow");
    api->fmt(S.status, sizeof S.status, "%s -- enter to connect", S.mode);
    return 1;
  default:
    return 0;
  }
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Screen",
  /* 16x16: a monitor on a stand. */
  { 0x00, 0x00, 0x7F, 0xFE, 0x40, 0x02, 0x5F, 0xF2,
    0x50, 0x12, 0x57, 0xD2, 0x54, 0x52, 0x54, 0x52,
    0x57, 0xD2, 0x50, 0x12, 0x5F, 0xF2, 0x40, 0x02,
    0x7F, 0xFE, 0x07, 0xE0, 0x1F, 0xF8, 0x00, 0x00 },
  "enter\tconnect\nf\tfollow the mouse, or the whole screen\n"
  "any key\tstop the stream\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  api->mem_set(&S, 0, sizeof S);

  api->fmt(S.base, sizeof S.base, "%s", CAPP_PROXY_DEFAULT);
  api->fmt(S.mode, sizeof S.mode, "%s", "follow");
  if (argc > 1 && argv[1][0]) api->fmt(S.base, sizeof S.base, "%s", argv[1]);
  api->fmt(S.status, sizeof S.status, "%s", "enter to connect");

  UI.paint = app_paint;
  UI.key = app_key;
  api->ui(&UI);
  return 0;
}
