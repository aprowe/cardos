/* Music: an MP3 player in the shape of the old iPod nano.
 *
 * The tracks are the ones dragged onto the dashboard -- the server makes
 * each a mono MP3 (server/music.py) and the kernel decodes it as it plays
 * (kernel/sys/mp3stream.c) -- plus any MP3 or WAV put in /music by hand
 * (USB disk mode), named by its tag or its file. Opening this shows what is
 * on the card at once, then syncs: the list, then each track not here yet,
 * streamed to the card ("2 of 5 40%" in the title bar); a key stops it. A
 * track gone from the server goes from /music. Synced tracks are
 * /music/<id>.mp3 (or .wav, from before), their titles in /music/.tracks.
 *
 * The screens are the nano's: a menu (Now Playing, Songs, Artists, Shuffle
 * Songs, Shuffle, Sync), lists with a blue bar and chevrons, and Now
 * Playing -- "3 of 12", a cover, the song, the artist, a bar with the time
 * gone and the time left. Up/down there are the volume, as the wheel was.
 * Choosing a song plays it and the rest of that list after it.
 *
 * Playing is the kernel's audio task (api->audio). Leaving the app lets
 * the track that is playing finish; nothing follows it.
 *
 * @app folder Make
 * @app release core
 */
#include "kernel/app/capp.h"
#include "apps/datetime.h"
#include "apps/str.h"
#include "apps/confirm.h"
#include "apps/safefile.h"
#include "apps/syncset.h"
#include "apps/id3.h"

static const CardApi *api;
static const CappAudio *au;

#define DIR       "/music"
#define INDEX     DIR "/.tracks"
#define MAXT      64
#define TITLE     40
#define ARTIST    28
#define IDL       40
#define TOP_H     19               /* the title bar, its line included */
#define ROW_H     19
#define TICK_MS   200
#define VOL_SHOW  1500             /* ms the volume bar stays after a change */

/* The nano's colours. */
#define C_BAR     CAPP_RGB(230, 232, 236)
#define C_BAR_HI  CAPP_RGB(250, 251, 252)
#define C_BAR_LO  CAPP_RGB(150, 156, 166)
#define C_INK     CAPP_RGB(24, 26, 30)
#define C_BG      CAPP_RGB(255, 255, 255)
#define C_DIM     CAPP_RGB(112, 116, 126)
#define C_CHEV    CAPP_RGB(160, 164, 172)
#define C_SEL     CAPP_RGB(52, 118, 220)
#define C_SEL_HI  CAPP_RGB(118, 172, 244)
#define C_SEL_LO  CAPP_RGB(28, 80, 176)
#define C_TRACK   CAPP_RGB(214, 217, 222)
#define C_EDGE    CAPP_RGB(150, 155, 165)
#define C_FILL    CAPP_RGB(64, 132, 230)
#define C_FILL_HI CAPP_RGB(146, 192, 250)
#define C_BAD     CAPP_RGB(200, 40, 40)
#define C_ASK     CAPP_RGB(40, 44, 52)

enum { V_MAIN, V_SONGS, V_ARTISTS, V_ARTIST, V_NOW, V_N };
enum { M_NOW, M_SONGS, M_ARTISTS, M_SHUFFLE_SONGS, M_SHUFFLE, M_SYNC };
enum { ACT_SYNC = 1, ACT_SHUFFLE, ACT_DELETE, ACT_NOW };

static struct {
  /* the tracks */
  char     id[MAXT][IDL];          /* the server's id, or a local file's name */
  char     ext[MAXT][4];           /* "mp3" or "wav" */
  uint8_t  local[MAXT];            /* put on the card by hand: not the server's */
  char     title[MAXT][TITLE];
  char     artist[MAXT][ARTIST];
  uint32_t ms[MAXT];
  int      n;
  /* what is on screen */
  int      view, back;             /* back: the list Now Playing came from */
  int      sel[V_N], top[V_N];
  uint8_t  songs[MAXT];            /* every track, A-Z */
  uint8_t  arts[MAXT];             /* a track per artist, A-Z by artist */
  int      narts;
  uint8_t  in_art[MAXT];           /* the songs of the artist chosen */
  int      nin;
  int      menu[8], nmenu;
  /* the queue: what plays next */
  uint8_t  q[MAXT];
  int      nq, qi;
  int      playing;                /* a track index, or -1 */
  int      stopped;                /* the user stopped it: do not go on */
  int      shuffle;
  uint32_t seed;
  /* what paint shows, taken in tick: paint runs once per strip, so it
   * must not ask the audio task (it would move between strips) */
  uint32_t pos, tot;
  int      paused, live, vol_on;
  uint32_t shown_s, vol_until;
  int      ask_delete, del_track;
  int      sync_soon;
  char     status[40];
  int      bad;
  uint32_t next_tick;
  CRect    c;
  int      have_c;
  int      f, fb, fbig;            /* pod13, pod13b, pod16b */
  char     reply[MAXT * 90];
  /* a download under way */
  int      fd, got, want, last_drawn, cancelled, nth, of;
} M;

/* ---- small things ---------------------------------------------------------------- */

static void path_of(int i, char *out, int n) {
  api->fmt(out, (size_t)n, DIR "/%s.%s", M.id[i], M.ext[i]);
}

static void say(int bad, const char *s) {
  api->fmt(M.status, sizeof M.status, "%s", s);
  M.bad = bad;
}

static long to_num(const char *s) {
  long v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* A-Z, case aside, and "The " aside as the iPod did. */
static int cmp_names(const char *a, const char *b) {
  if (lower(a[0]) == 't' && lower(a[1]) == 'h' && lower(a[2]) == 'e' && a[3] == ' ') a += 4;
  if (lower(b[0]) == 't' && lower(b[1]) == 'h' && lower(b[2]) == 'e' && b[3] == ' ') b += 4;
  while (*a && lower(*a) == lower(*b)) { a++; b++; }
  return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static const char *artist_of(int i) { return M.artist[i][0] ? M.artist[i] : "Unknown Artist"; }

static void sort_by(uint8_t *ix, int n, int by_artist) {
  int i, j;
  for (i = 1; i < n; i++) {
    uint8_t v = ix[i];
    for (j = i; j > 0; j--) {
      int c = by_artist ? cmp_names(artist_of(ix[j - 1]), artist_of(v)) : 0;
      if (!c) c = cmp_names(M.title[ix[j - 1]], M.title[v]);
      if (c <= 0) break;
      ix[j] = ix[j - 1];
    }
    ix[j] = v;
  }
}

/* The lists drawn from the tracks: after anything changes them. */
static void rebuild(void) {
  int i, j;
  for (i = 0; i < M.n; i++) M.songs[i] = (uint8_t)i;
  sort_by(M.songs, M.n, 0);
  M.narts = 0;
  for (i = 0; i < M.n; i++) {
    for (j = 0; j < M.narts; j++) if (!cmp_names(artist_of(M.arts[j]), artist_of(i))) break;
    if (j == M.narts) M.arts[M.narts++] = (uint8_t)i;
  }
  sort_by(M.arts, M.narts, 1);
  for (i = 0; i < V_N; i++) M.sel[i] = M.top[i] = 0;
  M.nin = 0;
  if (M.view == V_ARTIST) M.view = V_ARTISTS;
}

static void choose_artist(int a) {
  int i;
  M.nin = 0;
  for (i = 0; i < M.n; i++) if (!cmp_names(artist_of(i), artist_of(M.arts[a]))) M.in_art[M.nin++] = (uint8_t)i;
  sort_by(M.in_art, M.nin, 0);
  M.sel[V_ARTIST] = M.top[V_ARTIST] = 0;
}

/* ---- the index, and what is on the card ------------------------------------------ */

static void index_load(void) {
  int fd = safe_open_read(api, INDEX), n;
  const char *p;
  M.n = 0;
  if (fd < 0) return;
  n = api->read(fd, M.reply, sizeof M.reply - 1);
  api->close(fd);
  if (n <= 0) return;
  M.reply[n] = 0;
  for (p = M.reply; *p && M.n < MAXT; p = tsv_next_line(p)) {
    char ms[12], path[64];
    CappStat st;
    tsv_field(p, 0, M.id[M.n], sizeof M.id[M.n]);
    tsv_field(p, 1, M.title[M.n], TITLE);
    tsv_field(p, 2, ms, sizeof ms);
    tsv_field(p, 3, M.ext[M.n], sizeof M.ext[M.n]);
    tsv_field(p, 4, M.artist[M.n], ARTIST);
    if (!str_same(M.ext[M.n], "mp3")) api->fmt(M.ext[M.n], sizeof M.ext[0], "wav");   /* from before */
    M.ms[M.n] = (uint32_t)to_num(ms);
    path_of(M.n, path, sizeof path);
    M.local[M.n] = 0;
    if (M.id[M.n][0] && api->stat(path, &st) == 0) M.n++;   /* only what is here */
  }
}

/* A WAV's length from its header. An MP3's is learnt when it plays. */
static uint32_t wav_ms(const char *path, uint32_t size) {
  uint8_t h[44];
  uint32_t rate, sec, rem;
  int fd = api->open(path, CAPP_O_READ), n;
  if (fd < 0) return 0;
  n = api->read(fd, h, sizeof h);
  api->close(fd);
  if (n < 44) return 0;
  rate = (uint32_t)h[28] | (uint32_t)h[29] << 8 | (uint32_t)h[30] << 16 | (uint32_t)h[31] << 24;
  if (!rate || size < 44) return 0;
  size -= 44;
  sec = size / rate;
  rem = size % rate;
  return sec * 1000 + rem * 1000 / rate;  /* rem < rate < 400000: no overflow */
}

/* MP3s and WAVs put in /music by hand -- over USB disk mode, say -- that
 * the server does not know: named by their tags (or file names), never
 * removed by a sync, and deleted from the card only. */
static void add_local(void) {
  static char names[MAXT][IDL + 8];
  static uint8_t head[1024];
  int n = api->list(DIR, &names[0][0], MAXT, IDL + 8), i, j;
  for (i = 0; i < n && M.n < MAXT; i++) {
    char path[64], ext[4];
    CappStat st;
    int len = (int)api->str_len(names[i]), k;
    if (len < 5 || len - 4 >= IDL || names[i][0] == '.' || names[i][len - 4] != '.') continue;
    for (k = 0; k < 3; k++) ext[k] = (char)lower(names[i][len - 3 + k]);
    ext[3] = 0;
    if (!str_same(ext, "wav") && !str_same(ext, "mp3")) continue;
    names[i][len - 4] = 0;
    for (j = 0; j < M.n; j++) if (str_same(M.id[j], names[i])) break;
    if (j < M.n) continue;
    /* FAT ignores case, so NAME.MP3 opens as NAME.mp3 too. */
    api->fmt(path, sizeof path, DIR "/%s.%s", names[i], ext);
    if (api->stat(path, &st) != 0) continue;
    api->fmt(M.id[M.n], sizeof M.id[0], "%s", names[i]);
    api->fmt(M.ext[M.n], sizeof M.ext[0], "%s", ext);
    api->fmt(M.title[M.n], TITLE, "%s", names[i]);
    M.artist[M.n][0] = 0;
    M.ms[M.n] = 0;
    if (ext[0] == 'w') M.ms[M.n] = wav_ms(path, (uint32_t)st.size);
    else {
      int fd = api->open(path, CAPP_O_READ), got = 0;
      if (fd >= 0) { got = api->read(fd, head, sizeof head); api->close(fd); }
      if (got > 0) {
        if (!id3_text(head, got, "TIT2", M.title[M.n], TITLE)) api->fmt(M.title[M.n], TITLE, "%s", names[i]);
        id3_text(head, got, "TPE1", M.artist[M.n], ARTIST);
      }
    }
    M.local[M.n] = 1;
    M.n++;
  }
}

static void index_save(void) {
  SafeFile f;
  int i;
  if (safe_begin(&f, api, INDEX) != 0) return;
  for (i = 0; i < M.n; i++) {
    char line[128];
    int k;
    if (M.local[i]) continue;
    k = api->fmt(line, sizeof line, "%s\t%s\t%lu\t%s\t%s\n", M.id[i], M.title[i],
                 (unsigned long)M.ms[i], M.ext[i], M.artist[i]);
    safe_write(&f, line, (size_t)k);
  }
  safe_commit(&f);
}

/* ---- drawing pieces --------------------------------------------------------------- */

static void fill(int x, int y, int w, int h, uint16_t c) {
  if (w > 0 && h > 0) api->fill(capp_rect(x, y, w, h), c);
}

/* `s` cut to fit `w` pixels in font `f`, with "..." when cut. */
static void fit(int f, const char *s, int w, char *out, int n) {
  int len = (int)api->str_len(s);
  api->fmt(out, (size_t)n, "%s", s);
  if (api->text_width(f, out) <= w) return;
  while (len > 0) {
    len--;
    api->fmt(out, (size_t)n, "%.*s...", len, s);
    if (api->text_width(f, out) <= w) return;
  }
}

/* Text in a band of `h` rows from y, centred in it; the band is filled. */
static void text_in(int f, int x, int y, int h, const char *s, uint16_t fg, uint16_t bg) {
  int fh = api->font_height(f);
  api->text_font(f, (int16_t)x, (int16_t)(y + (h - fh) / 2), s, fg, bg);
}

static void chevron(int x, int cy, uint16_t c) {
  int i;
  for (i = 0; i < 4; i++) {
    fill(x + i, cy - 4 + i, 2, 1, c);
    fill(x + i, cy + 4 - i, 2, 1, c);
  }
  fill(x + 4, cy, 2, 1, c);
}

static void play_glyph(int x, int cy, int paused, uint16_t c) {
  int i;
  if (paused) { fill(x, cy - 4, 3, 9, c); fill(x + 5, cy - 4, 3, 9, c); return; }
  for (i = 0; i < 6; i++) fill(x + i, cy - 5 + i, 1, 11 - 2 * i, c);
}

/* The title bar: the name in the middle, play or pause at the left while
 * something plays, and what is going on at the right. */
static void paint_bar(const char *title) {
  CRect c = M.c;
  char s[40];
  int w, tw;
  fill(c.x, c.y, c.w, 1, C_BAR_HI);
  fill(c.x, c.y + 1, c.w, TOP_H - 2, C_BAR);
  fill(c.x, c.y + TOP_H - 1, c.w, 1, C_BAR_LO);
  tw = api->text_width(M.fb, title);
  text_in(M.fb, c.x + (c.w - tw) / 2, c.y + 1, TOP_H - 2, title, C_INK, C_BAR);
  if (M.live) play_glyph(c.x + 7, c.y + TOP_H / 2, M.paused, C_INK);
  if (M.status[0]) {
    fit(M.f, M.status, (c.w - tw) / 2 - 10, s, sizeof s);
    w = api->text_width(M.f, s);
    text_in(M.f, c.x + c.w - 5 - w, c.y + 1, TOP_H - 2, s, M.bad ? C_BAD : C_DIM, C_BAR);
  } else if (M.shuffle && M.view != V_NOW) {
    w = api->text_width(M.f, "shuffle");
    text_in(M.f, c.x + c.w - 5 - w, c.y + 1, TOP_H - 2, "shuffle", C_DIM, C_BAR);
  }
}

/* ---- the lists ----------------------------------------------------------------------- */

static int list_rows(void) { return (M.c.h - TOP_H) / ROW_H; }

static int list_len(int v) {
  return v == V_MAIN ? M.nmenu : v == V_SONGS ? M.n : v == V_ARTISTS ? M.narts : v == V_ARTIST ? M.nin : 0;
}

static int track_at(int v, int i) {
  return v == V_SONGS ? M.songs[i] : v == V_ARTIST ? M.in_art[i] : v == V_ARTISTS ? M.arts[i] : -1;
}

static void build_menu(void) {
  /* Now Playing last, as on the nano: coming and going, it moves nothing
   * under the cursor. */
  M.nmenu = 0;
  M.menu[M.nmenu++] = M_SONGS;
  M.menu[M.nmenu++] = M_ARTISTS;
  M.menu[M.nmenu++] = M_SHUFFLE_SONGS;
  M.menu[M.nmenu++] = M_SHUFFLE;
  M.menu[M.nmenu++] = M_SYNC;
  if (M.playing >= 0) M.menu[M.nmenu++] = M_NOW;
  if (M.sel[V_MAIN] >= M.nmenu) M.sel[V_MAIN] = M.nmenu - 1;
}

static void row_label(int v, int i, char *label, int n, const char **value, int *more) {
  static const char *const NAMES[] = { "Now Playing", "Songs", "Artists", "Shuffle Songs", "Shuffle", "Sync" };
  *value = "";
  *more = 0;
  if (v == V_MAIN) {
    int m = M.menu[i];
    api->fmt(label, (size_t)n, "%s", NAMES[m]);
    if (m == M_SHUFFLE) *value = M.shuffle ? "On" : "Off";
    else if (m == M_SYNC) *value = "";
    else if (m != M_SHUFFLE_SONGS) *more = 1;
  } else if (v == V_ARTISTS) {
    api->fmt(label, (size_t)n, "%s", artist_of(M.arts[i]));
    *more = 1;
  } else {
    api->fmt(label, (size_t)n, "%s", M.title[track_at(v, i)]);
  }
}

static void paint_row(int v, int i, int y, int w) {
  char label[48], s[48];
  const char *value;
  int more, sel = i == M.sel[v], x = M.c.x, right = x + w;
  uint16_t bg = sel ? C_SEL : C_BG, fg = sel ? C_BG : C_INK;
  int t = track_at(v, i);
  row_label(v, i, label, sizeof label, &value, &more);
  if (sel) {
    fill(x, y, w, 1, C_SEL_HI);
    fill(x, y + 1, w, ROW_H - 2, C_SEL);
    fill(x, y + ROW_H - 1, w, 1, C_SEL_LO);
  } else {
    fill(x, y, w, ROW_H, C_BG);
  }
  if (more) { chevron(right - 12, y + ROW_H / 2, sel ? C_BG : C_CHEV); right -= 16; }
  else if (value[0]) {
    int vw = api->text_width(M.f, value);
    text_in(M.f, right - 8 - vw, y + 1, ROW_H - 2, value, sel ? C_BG : C_DIM, bg);
    right -= vw + 14;
  } else if (v != V_MAIN && t >= 0 && t == M.playing && v != V_ARTISTS) {
    play_glyph(right - 13, y + ROW_H / 2, M.paused, sel ? C_BG : C_SEL);
    right -= 18;
  }
  fit(sel ? M.fb : M.f, label, right - x - 10, s, sizeof s);
  text_in(sel ? M.fb : M.f, x + 8, y + 1, ROW_H - 2, s, fg, bg);
}

static void paint_list(int v) {
  int rows = list_rows(), n = list_len(v), i, y = M.c.y + TOP_H, w = M.c.w;
  if (M.sel[v] >= n) M.sel[v] = n ? n - 1 : 0;
  if (M.sel[v] < M.top[v]) M.top[v] = M.sel[v];
  if (M.sel[v] >= M.top[v] + rows) M.top[v] = M.sel[v] - rows + 1;
  if (n > rows) w -= 5;                                  /* the scrollbar's */
  for (i = M.top[v]; i < n && i - M.top[v] < rows; i++, y += ROW_H) paint_row(v, i, y, w);
  fill(M.c.x, y, w, M.c.y + M.c.h - y, C_BG);
  if (n > rows) {
    int track = M.c.h - TOP_H - 2, th = track * rows / n, ty = track * M.top[v] / n;
    if (th < 8) th = 8;
    fill(M.c.x + w, M.c.y + TOP_H, 5, M.c.h - TOP_H, C_BG);
    fill(M.c.x + w + 1, M.c.y + TOP_H + 1, 3, track, C_TRACK);
    fill(M.c.x + w + 1, M.c.y + TOP_H + 1 + ty, 3, th, C_SEL);
  }
  if (!n && v != V_MAIN) {
    int y0 = M.c.y + TOP_H + 26;
    const char *a = "No songs", *b = M.status[0] ? M.status : "Add music on the dashboard";
    text_in(M.fb, M.c.x + (M.c.w - api->text_width(M.fb, a)) / 2, y0, 20, a, C_INK, C_BG);
    text_in(M.f, M.c.x + (M.c.w - api->text_width(M.f, b)) / 2, y0 + 22, 18, b, C_DIM, C_BG);
  }
}

/* ---- Now Playing ------------------------------------------------------------------------- */

/* A cover, since there are none: a colour from the song's name, and a note. */
static void paint_art(int x, int y, int s, int t) {
  static const uint16_t TOPS[] = {
    CAPP_RGB(232, 92, 92), CAPP_RGB(240, 150, 60), CAPP_RGB(80, 170, 110), CAPP_RGB(70, 140, 230),
    CAPP_RGB(150, 100, 220), CAPP_RGB(220, 90, 160), CAPP_RGB(60, 170, 180), CAPP_RGB(120, 128, 140),
  };
  static const uint16_t BOTS[] = {
    CAPP_RGB(150, 40, 50), CAPP_RGB(170, 80, 20), CAPP_RGB(30, 100, 60), CAPP_RGB(30, 70, 150),
    CAPP_RGB(80, 40, 140), CAPP_RGB(140, 40, 90), CAPP_RGB(20, 100, 110), CAPP_RGB(60, 64, 74),
  };
  static const uint8_t HEAD[] = { 3, 5, 6, 6, 6, 6, 5, 3 };
  const char *p = M.artist[t][0] ? M.artist[t] : M.title[t];
  uint32_t h = 2166136261u;
  int k, i, half = s / 2, nx = x + s / 2 - 6, ny = y + s / 2 - 12;
  while (*p) h = (h ^ (uint8_t)lower(*p++)) * 16777619u;
  k = (int)(h % 8);
  fill(x, y, s, half, TOPS[k]);
  fill(x, y + half, s, s - half, BOTS[k]);
  fill(x, y + half - 2, s, 4, TOPS[k] == BOTS[k] ? TOPS[k] : BOTS[k]);
  /* the note: a stem, a flag, a head */
  fill(nx + 8, ny, 3, 20, C_BG);
  for (i = 0; i < 6; i++) fill(nx + 11, ny + i * 2, 6 - i, 2, C_BG);
  for (i = 0; i < 8; i++) fill(nx + 5 - HEAD[i], ny + 16 + i, HEAD[i] * 2, 1, C_BG);   /* left of the stem */
}

static void paint_now(void) {
  CRect c = M.c;
  char s[48], a[12], b[12];
  int t = M.playing, x = c.x + 72, w = c.w - 72 - 6, by = c.y + 103, bw = c.w - 16, i;
  fill(c.x, c.y + TOP_H, c.w, c.h - TOP_H, C_BG);
  if (t < 0) {
    text_in(M.fb, c.x + (c.w - api->text_width(M.fb, "Nothing playing")) / 2, c.y + 50, 20,
            "Nothing playing", C_INK, C_BG);
    return;
  }
  api->fmt(s, sizeof s, "%d of %d", M.qi + 1, M.nq);
  text_in(M.f, c.x + 8, c.y + TOP_H + 1, 17, s, C_DIM, C_BG);
  if (M.shuffle) {
    int sw = api->text_width(M.f, "Shuffle");
    text_in(M.f, c.x + c.w - 8 - sw, c.y + TOP_H + 1, 17, "Shuffle", C_DIM, C_BG);
  }
  paint_art(c.x + 8, c.y + 40, 56, t);
  fit(M.fbig, M.title[t], w, s, sizeof s);
  text_in(M.fbig, x, c.y + 40, 20, s, C_INK, C_BG);
  fit(M.f, artist_of(t), w, s, sizeof s);
  text_in(M.f, x, c.y + 61, 17, s, C_INK, C_BG);
  fit(M.f, M.local[t] ? "On this card" : "From the dashboard", w, s, sizeof s);
  text_in(M.f, x, c.y + 78, 17, s, C_DIM, C_BG);

  if (M.vol_on) {
    /* the nano's volume bar: a speaker, the bar, a louder speaker */
    int v = au ? au->volume() : 0, fx = c.x + 24, fw = bw - 32;
    fill(c.x + 8, by + 1, 3, 5, C_DIM);
    for (i = 0; i < 4; i++) fill(c.x + 11 + i, by - i + 1, 1, 5 + 2 * i, C_DIM);
    fill(fx, by, fw, 7, C_EDGE);
    fill(fx + 1, by + 1, fw - 2, 5, C_TRACK);
    fill(fx + 1, by + 1, (fw - 2) * v / 100, 2, C_FILL_HI);
    fill(fx + 1, by + 3, (fw - 2) * v / 100, 3, C_FILL);
    api->fmt(s, sizeof s, "Volume %d", v);
    text_in(M.f, c.x + (c.w - api->text_width(M.f, s)) / 2, by + 10, 17, s, C_DIM, C_BG);
    return;
  }
  {
    uint32_t tot = M.tot ? M.tot : M.ms[t], pos = M.pos > tot ? tot : M.pos;
    /* 32-bit: an app has no 64-bit division, and the width times half an
     * hour in milliseconds is still under 2^32. */
    int fw = tot ? (int)((uint32_t)(bw - 2) * pos / tot) : 0;
    fill(c.x + 8, by, bw, 7, C_EDGE);
    fill(c.x + 9, by + 1, bw - 2, 5, C_TRACK);
    fill(c.x + 9, by + 1, fw, 2, C_FILL_HI);
    fill(c.x + 9, by + 3, fw, 3, C_FILL);
    dt_mmss(api, pos, a, sizeof a);
    b[0] = '-';
    dt_mmss(api, tot - pos, b + 1, sizeof b - 1);
    text_in(M.f, c.x + 8, by + 10, 17, a, C_INK, C_BG);
    text_in(M.f, c.x + c.w - 8 - api->text_width(M.f, b), by + 10, 17, b, C_INK, C_BG);
    if (M.paused) {
      const char *pz = "Paused";
      text_in(M.f, c.x + (c.w - api->text_width(M.f, pz)) / 2, by + 10, 17, pz, C_DIM, C_BG);
    }
  }
}

static CRect now_bottom(void) { return capp_rect(M.c.x, M.c.y + 100, M.c.w, M.c.h - 100); }

static void paint_ask(void) {
  CRect c = M.c;
  char s[64], t[48];
  int t_i = M.del_track;
  fit(M.f, t_i >= 0 ? M.title[t_i] : "", 120, t, sizeof t);
  api->fmt(s, sizeof s, "Delete %s?  y / n", t);
  fill(c.x, c.y + c.h - 22, c.w, 22, C_ASK);
  text_in(M.f, c.x + 8, c.y + c.h - 21, 20, s, C_BG, C_ASK);
}

static void app_paint(void *st, CRect c) {
  static const char *const TITLES[] = { "Music", "Songs", "Artists", "", "Now Playing" };
  (void)st;
  M.c = c;
  M.have_c = 1;
  build_menu();
  paint_bar(M.view == V_ARTIST && M.nin ? artist_of(M.in_art[0]) : TITLES[M.view]);
  if (M.view == V_NOW) paint_now();
  else paint_list(M.view);
  if (M.ask_delete) paint_ask();
}

static void mark_all(void) { if (M.have_c) api->damage(M.c); }

/* What paint shows of the audio, read once. */
static void snap(void) {
  int s = au ? au->state() : CAPP_AUDIO_IDLE;
  M.live = M.playing >= 0 && s == CAPP_AUDIO_PLAYING;
  M.paused = M.live && au->paused();
  M.pos = M.live ? au->pos_ms() : 0;
  M.tot = M.live ? au->total_ms() : 0;
  M.shown_s = M.pos / 1000;
  M.vol_on = M.vol_until && (int32_t)(api->ticks_ms() - M.vol_until) < 0;
}

/* ---- playing -------------------------------------------------------------------------- */

static void play(int i) {
  char path[64];
  int rc;
  if (!au || i < 0 || i >= M.n) return;
  if (au->state() != CAPP_AUDIO_IDLE) au->stop();
  /* The audio task lets go on its own time: wait a moment for it. */
  for (rc = 0; rc < 40 && au->state() != CAPP_AUDIO_IDLE; rc++) {
    uint32_t t = api->ticks_ms() + 10;
    while ((int32_t)(api->ticks_ms() - t) < 0) ;
  }
  path_of(i, path, sizeof path);
  rc = au->play(path);
  if (rc != 0) {
    char line[96];
    api->fmt(line, sizeof line, "%s would not play: %s", path, au->error());
    api->log(line);
    say(1, au->error()[0] ? au->error() : "would not play");
    M.playing = -1;
    return;
  }
  M.playing = i;
  M.stopped = 0;
  if (au->total_ms()) M.ms[i] = au->total_ms();     /* an MP3's length, known now */
  say(0, "");
}

/* Play `list` from `at`, and the rest of it after. */
static void play_list(const uint8_t *list, int n, int at) {
  int i;
  if (!n) return;
  for (i = 0; i < n; i++) M.q[i] = list[i];
  M.nq = n;
  M.qi = at;
  play(M.q[at]);
}

static int rnd(int n) {
  M.seed = M.seed * 1664525u + 1013904223u;
  return (int)((M.seed >> 8) % (uint32_t)n);
}

static void step(int dir) {
  if (!M.nq) return;
  if (M.shuffle && M.nq > 1) {
    int k = rnd(M.nq - 1);
    M.qi = k >= M.qi ? k + 1 : k;                      /* never the same one */
  } else {
    M.qi = (M.qi + dir + M.nq) % M.nq;
  }
  play(M.q[M.qi]);
}

static void stop_playing(void) {
  if (M.playing >= 0 && au) { M.stopped = 1; au->stop(); }
}

/* After the tracks change under the queue: all songs, the one playing
 * found again by its id. */
static void requeue(const char *was) {
  int i;
  M.playing = -1;
  for (i = 0; i < M.n; i++) M.q[i] = M.songs[i];
  M.nq = M.n;
  M.qi = 0;
  for (i = 0; i < M.n && was[0]; i++)
    if (str_same(M.id[M.q[i]], was)) { M.playing = M.q[i]; M.qi = i; break; }
}

/* ---- sync -------------------------------------------------------------------------------- */

/* While a track streams in: to the card, and the title bar says how far --
 * the shell is not painting while this runs. Any key stops it. */
static int on_data(void *ctx, const uint8_t *d, int n) {
  (void)ctx;
  if (api->write(M.fd, d, (size_t)n) != n) return 1;
  M.got += n;
  if (M.have_c && (M.got - M.last_drawn >= 32 * 1024 || M.got >= M.want)) {
    int pct = M.want >= 100 ? M.got / (M.want / 100) : 0;
    if (pct > 100) pct = 100;
    api->fmt(M.status, sizeof M.status, "%d of %d  %d%%", M.nth, M.of, pct);
    M.bad = 0;
    paint_bar(M.view == V_NOW ? "Now Playing" : "Syncing");
    M.last_drawn = M.got;
  }
  if (api->key_pending()) { M.cancelled = 1; return 1; }
  return 0;
}

static void sync_run(void) {
  static char ids[MAXT][12], titles[MAXT][TITLE], artists[MAXT][ARTIST], exts[MAXT][4];
  static uint32_t ms[MAXT], bytes[MAXT];
  char url[160], path[64], part[64], was[IDL];
  const char *p;
  int r, n = 0, i, j, fetched = 0, whole, kept;

  if (!api->net_ready() && api->net_connect(15000) != 0) { say(0, "offline"); mark_all(); return; }
  api->fmt(url, sizeof url, "%s/music", api->proxy());
  r = api->http("GET", url, 0, 0, "", M.reply, sizeof M.reply, 15000);
  if (r < 0 || (M.reply[0] == 'e' && M.reply[1] == 'r')) { say(1, "no answer"); mark_all(); return; }
  whole = sync_reply(M.reply, r, (int)sizeof M.reply);
  for (p = M.reply; *p && n < MAXT; p = tsv_next_line(p)) {
    char num[16];
    tsv_field(p, 0, ids[n], sizeof ids[n]);
    tsv_field(p, 1, titles[n], TITLE);
    tsv_field(p, 2, num, sizeof num); ms[n] = (uint32_t)to_num(num);
    tsv_field(p, 3, num, sizeof num); bytes[n] = (uint32_t)to_num(num);
    tsv_field(p, 4, exts[n], sizeof exts[n]);
    tsv_field(p, 5, artists[n], ARTIST);
    if (!str_same(exts[n], "mp3")) api->fmt(exts[n], sizeof exts[0], "wav");
    if (ids[n][0]) n++;
  }
  /* Gone from the server: gone from here (not the one playing) -- but only
   * against the whole list. One that filled the reply, or ran to MAXT, is
   * not all of it, and what it leaves out stays (apps/syncset.h). */
  if (sync_may_delete(whole, n, MAXT)) {
    for (i = 0; i < M.n; i++) {
      if (M.local[i] || i == M.playing || sync_listed(M.id[i], &ids[0][0], n, (int)sizeof ids[0])) continue;
      path_of(i, path, sizeof path);
      api->remove(path);
    }
  }
  api->mkdir(DIR);
  /* The rest, each streamed to NAME.part and renamed only when whole. A
   * track the server made MP3 since replaces its WAV. */
  M.of = 0;
  for (j = 0; j < n; j++) {
    CappStat st;
    api->fmt(path, sizeof path, DIR "/%s.%s", ids[j], exts[j]);
    if (api->stat(path, &st) != 0) M.of++;
  }
  M.nth = 0;
  for (j = 0; j < n && !M.cancelled; j++) {
    CappStat st;
    api->fmt(path, sizeof path, DIR "/%s.%s", ids[j], exts[j]);
    if (api->stat(path, &st) == 0) continue;
    M.nth = fetched + 1;
    api->fmt(M.status, sizeof M.status, "%d of %d", M.nth, M.of);
    if (M.have_c) paint_bar(M.view == V_NOW ? "Now Playing" : "Syncing");
    api->fmt(part, sizeof part, DIR "/%s.part", ids[j]);
    M.fd = api->open(part, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
    if (M.fd < 0) { say(1, "card refused"); break; }
    M.got = M.last_drawn = 0;
    M.want = (int)bytes[j];
    api->fmt(url, sizeof url, "%s/music/track?id=%s", api->proxy(), ids[j]);
    r = api->http_stream(url, on_data, 0, 60000);
    api->close(M.fd);
    if (r < 0 || M.got != M.want) { api->remove(part); if (!M.cancelled) say(1, "cut short: r again"); break; }
    api->rename(part, path);
    if (str_same(exts[j], "mp3")) {
      api->fmt(part, sizeof part, DIR "/%s.wav", ids[j]);
      for (i = 0; i < M.n; i++) if (str_same(M.id[i], ids[j]) && i == M.playing) break;
      if (i == M.n) api->remove(part);               /* not while it plays */
    }
    fetched++;
  }
  /* The list is the server's, as far as the card has it -- and with a
   * partial list, the tracks it did not mention that are still here. */
  was[0] = 0;
  if (M.playing >= 0 && M.playing < M.n) api->fmt(was, sizeof was, "%s", M.id[M.playing]);
  kept = 0;
  if (!sync_may_delete(whole, n, MAXT)) {
    for (i = 0; i < M.n && n + kept < MAXT; i++) {
      if (M.local[i] || sync_listed(M.id[i], &ids[0][0], n, (int)sizeof ids[0])) continue;
      if (kept != i) {
        api->mem_cpy(M.id[kept], M.id[i], sizeof M.id[0]);
        api->mem_cpy(M.title[kept], M.title[i], sizeof M.title[0]);
        api->mem_cpy(M.artist[kept], M.artist[i], sizeof M.artist[0]);
        api->mem_cpy(M.ext[kept], M.ext[i], sizeof M.ext[0]);
        M.ms[kept] = M.ms[i];
      }
      M.local[kept] = 0;
      kept++;
    }
  }
  M.n = kept;
  for (j = 0; j < n && M.n < MAXT; j++) {
    CappStat st;
    api->fmt(path, sizeof path, DIR "/%s.%s", ids[j], exts[j]);
    if (api->stat(path, &st) != 0) {
      /* not fetched (yet): the old WAV still plays */
      api->fmt(path, sizeof path, DIR "/%s.wav", ids[j]);
      if (api->stat(path, &st) != 0) continue;
      api->fmt(M.ext[M.n], sizeof M.ext[0], "wav");
    } else {
      api->mem_cpy(M.ext[M.n], exts[j], sizeof M.ext[0]);
    }
    api->fmt(M.id[M.n], sizeof M.id[0], "%s", ids[j]);
    M.local[M.n] = 0;
    api->mem_cpy(M.title[M.n], titles[j], sizeof M.title[0]);
    api->mem_cpy(M.artist[M.n], artists[j], sizeof M.artist[0]);
    M.ms[M.n] = ms[j];
    M.n++;
  }
  index_save();
  add_local();
  {
    int v = M.view;
    rebuild();
    M.view = v == V_ARTIST ? V_ARTISTS : v;
  }
  requeue(was);
  if (M.cancelled) say(0, "sync stopped");
  else if (!M.bad) {
    if (fetched > 1) api->fmt(M.status, sizeof M.status, "%d new songs", fetched);
    else say(0, fetched ? "1 new song" : "");
  }
  M.cancelled = 0;
  snap();
  mark_all();
}

static void delete_track(int t) {
  char url[160], reply[32], path[64], was[IDL];
  int i;
  if (t < 0 || t >= M.n) return;
  api->fmt(url, sizeof url, "%s/music/track?id=%s", api->proxy(), M.id[t]);
  if (!M.local[t] &&
      (!api->net_ready() || api->http("DELETE", url, 0, 0, "", reply, sizeof reply, 15000) < 0)) {
    say(1, "offline");
    return;
  }
  if (t == M.playing) { stop_playing(); M.playing = -1; }
  was[0] = 0;
  if (M.playing >= 0) api->fmt(was, sizeof was, "%s", M.id[M.playing]);
  path_of(t, path, sizeof path);
  api->remove(path);
  for (i = t; i < M.n - 1; i++) {
    api->mem_cpy(M.id[i], M.id[i + 1], sizeof M.id[0]);
    api->mem_cpy(M.title[i], M.title[i + 1], sizeof M.title[0]);
    api->mem_cpy(M.artist[i], M.artist[i + 1], sizeof M.artist[0]);
    api->mem_cpy(M.ext[i], M.ext[i + 1], sizeof M.ext[0]);
    M.ms[i] = M.ms[i + 1];
    M.local[i] = M.local[i + 1];
  }
  M.n--;
  index_save();
  {
    int v = M.view, s = M.sel[v];
    rebuild();
    M.view = v == V_ARTIST ? V_ARTISTS : v;
    M.sel[M.view] = s;
  }
  requeue(was);
  say(0, "deleted");
}

/* ---- keys and time ------------------------------------------------------------------------- */

static int selected_track(void) {
  if (M.view == V_NOW) return M.playing;
  if (M.view == V_SONGS || M.view == V_ARTIST) return list_len(M.view) ? track_at(M.view, M.sel[M.view]) : -1;
  return -1;
}

static void go(int v) {
  if (v == V_NOW && M.view != V_NOW) M.back = M.view;
  M.view = v;
}

static int do_action(int a) {
  switch (a) {
  case ACT_SYNC: say(0, "syncing"); M.sync_soon = 1; return 1;
  case ACT_SHUFFLE: M.shuffle = !M.shuffle; return 1;
  case ACT_DELETE:
    M.del_track = selected_track();
    if (M.del_track >= 0) M.ask_delete = 1;
    return 1;
  case ACT_NOW: if (M.playing >= 0) go(V_NOW); return 1;
  }
  return 0;
}

static void choose(void) {
  int v = M.view, i = M.sel[v];
  if (v == V_MAIN) {
    switch (M.menu[i]) {
    case M_NOW: go(V_NOW); break;
    case M_SONGS: go(V_SONGS); break;
    case M_ARTISTS: go(V_ARTISTS); break;
    case M_SHUFFLE_SONGS:
      if (!M.n) { say(0, "no songs"); break; }
      M.shuffle = 1;
      play_list(M.songs, M.n, rnd(M.n));
      if (M.playing >= 0) go(V_NOW);
      break;
    case M_SHUFFLE: M.shuffle = !M.shuffle; break;
    case M_SYNC: do_action(ACT_SYNC); break;
    }
  } else if (v == V_ARTISTS) {
    if (!M.narts) return;
    choose_artist(i);
    go(V_ARTIST);
  } else if (v == V_SONGS || v == V_ARTIST) {
    if (!list_len(v)) return;
    play_list(v == V_SONGS ? M.songs : M.in_art, list_len(v), i);
    if (M.playing >= 0) go(V_NOW);
  } else if (v == V_NOW) {
    if (M.live) au->pause(!au->paused());
    else if (M.playing >= 0) play(M.playing);
  }
}

static void back(void) {
  if (M.view == V_NOW) M.view = M.back == V_NOW ? V_MAIN : M.back;
  else if (M.view == V_ARTIST) M.view = V_ARTISTS;
  else M.view = V_MAIN;
}

static void volume(int by) {
  int v = au->volume() + by;
  au->set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
  M.vol_until = api->ticks_ms() + VOL_SHOW;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (M.ask_delete) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_WAIT) return 1;
    M.ask_delete = 0;
    if (a == CONFIRM_YES) delete_track(M.del_track);
    snap();
    mark_all();
    return 1;
  }
  if (!au) return 0;
  switch (k) {
  case CAPP_KEY_UP:
    if (M.view == V_NOW) volume(+5);
    else if (M.sel[M.view] > 0) M.sel[M.view]--;
    break;
  case CAPP_KEY_DOWN:
    if (M.view == V_NOW) volume(-5);
    else if (M.sel[M.view] < list_len(M.view) - 1) M.sel[M.view]++;
    break;
  case CAPP_KEY_ENTER: choose(); break;
  case ' ':
    if (M.live) au->pause(!au->paused());
    else if (M.view == V_SONGS || M.view == V_ARTIST) choose();
    else if (M.playing >= 0) play(M.playing);
    else if (M.n) play_list(M.songs, M.n, 0);
    break;
  /* Ten seconds back or on: [ ], or shift with the arrows (< and ?). */
  case '[': case '<': case ']': case '?':
    if (M.live) {
      uint32_t at = au->pos_ms(), tot = au->total_ms();
      if (k == '[' || k == '<') at = at > 10000 ? at - 10000 : 0;
      else at = at + 10000 < tot ? at + 10000 : tot;
      au->seek_ms(at);
    }
    break;
  case 'x': case 'X': stop_playing(); break;
  case CAPP_KEY_LEFT:  if (M.playing >= 0) step(-1); else return 0; break;
  case CAPP_KEY_RIGHT: if (M.playing >= 0) step(1); else return 0; break;
  case '+': case '=':  volume(+10); if (M.view != V_NOW) say(0, ""); break;
  case '-': case '_':  volume(-10); if (M.view != V_NOW) say(0, ""); break;
  case 's': case 'S':  do_action(ACT_SHUFFLE); break;
  case 'r': case 'R':  do_action(ACT_SYNC); break;
  case 'n': case 'N':  do_action(ACT_NOW); break;
  case 'd': case 'D': case 0x7F: do_action(ACT_DELETE); break;
  case CAPP_KEY_ESC:
    if (M.view == V_MAIN) return 0;
    back();
    break;
  default: return 0;
  }
  if ((k == '+' || k == '=' || k == '-' || k == '_') && M.view != V_NOW)
    api->fmt(M.status, sizeof M.status, "Volume %d", au->volume());
  snap();
  mark_all();
  return 1;
}

static int app_action(void *st, int a) { (void)st; return do_action(a) && (snap(), mark_all(), 1); }

static int app_tick(void *st, uint32_t now) {
  int s, vol_on;
  (void)st;
  if (M.sync_soon && M.have_c) { M.sync_soon = 0; sync_run(); return 1; }
  if ((int32_t)(now - M.next_tick) < 0 || !au) return 0;
  M.next_tick = now + TICK_MS;
  s = au->state();
  /* A track ended by itself: the next one, unless the user stopped it. */
  if (M.playing >= 0 && s == CAPP_AUDIO_IDLE) {
    if (!M.stopped && M.nq) {
      if (!M.shuffle && M.qi == M.nq - 1) M.playing = -1;     /* the end of the list */
      else step(1);
    } else {
      M.playing = -1;
    }
    if (M.playing < 0 && M.view == V_NOW) M.view = M.back == V_NOW ? V_MAIN : M.back;
    snap();
    mark_all();
    return 1;
  }
  /* The volume bar going away, and Now Playing's clock, once a second. */
  vol_on = (int32_t)(now - M.vol_until) < 0;
  if (M.view == V_NOW && M.have_c &&
      ((s == CAPP_AUDIO_PLAYING && au->pos_ms() / 1000 != M.shown_s) || (!vol_on && M.vol_until))) {
    if (!vol_on) M.vol_until = 0;
    snap();
    api->damage(now_bottom());
    return 1;
  }
  if (M.view != V_NOW && M.vol_until && !vol_on) {
    M.vol_until = 0;
    if (M.status[0] == 'V') { say(0, ""); mark_all(); return 1; }
  }
  return 0;
}

static const CappAction ACTIONS[] = {
  { "sync", "Sync", 0, 0, ACT_SYNC, 0, 0, 0, 0 },
  { "shuffle", "Shuffle", 0, 0, ACT_SHUFFLE, 0, 0, 0, 0 },
  { "now", "Now Playing", 0, 0, ACT_NOW, 0, 0, 0, 0 },
  { "delete", "Delete song", 0, 0, ACT_DELETE, 0, 0, 0, 0 },
};

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Music",
  /* 16x16: two quavers. */
  { 0x00, 0x00, 0x03, 0xFE, 0x03, 0xFE, 0x03, 0x02,
    0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x1E, 0x1E, 0x3E, 0x3E, 0x3E,
    0x3E, 0x1C, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "up/down\tchoose (Now Playing: volume)\nenter\topen, play (Now Playing: pause)\nesc\tback a screen\n"
  "space\tpause, go on\nleft/right\tthe song before or after\n"
  "[ ]\tten seconds back, on (or shift+left/right)\n+ -\tvolume\ns\tshuffle on and off\n"
  "n\tNow Playing\nr\tsync with the dashboard\nd, del\tdelete the song (asks; from the dashboard too)\n"
  "x\tstop\n\nsongs come from the dashboard's Music page, or MP3s put in /music.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  au = api->audio();
  M.playing = -1;
  M.del_track = -1;
  M.view = V_MAIN;
  M.back = V_MAIN;
  M.seed = api->ticks_ms();
  M.f = api->font_load("pod13");
  M.fb = api->font_load("pod13b");
  M.fbig = api->font_load("pod16b");
  if (M.fb < 0) M.fb = M.f;
  if (M.fbig < 0) M.fbig = M.fb;
  api->mkdir(DIR);
  index_load();
  add_local();
  rebuild();
  requeue("");
  M.sync_soon = 1;                  /* after the first paint: the list shows first */
  if (au && au->state() == CAPP_AUDIO_PLAYING) say(0, "something is playing");
  build_menu();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.actions = ACTIONS;
  UI.nactions = sizeof ACTIONS / sizeof ACTIONS[0];
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
