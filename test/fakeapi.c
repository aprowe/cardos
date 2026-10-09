/* The CardApi host tests start from. See fakeapi.h. */

#include "fakeapi.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CappTime fake_now;
uint32_t fake_epoch;
uint32_t fake_ticks;
int      fake_repeat;
int      fake_headless;
int      fake_key_pending;
char     fake_out[4096];
CRect    fake_screen = { 0, 0, 240, 135 };

/* ---- the defaults ------------------------------------------------------------ */

static void d_fill(CRect r, uint16_t c) { (void)r; (void)c; }
static void d_frame(CRect r, uint16_t c) { (void)r; (void)c; }
static void d_bevel(CRect r, uint16_t f, uint16_t t, uint16_t b) { (void)r; (void)f; (void)t; (void)b; }
static void d_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  (void)x; (void)y; (void)s; (void)fg; (void)bg;
}
static void d_pixels(CRect r, const uint16_t *p) { (void)r; (void)p; }

static int  d_open(const char *p, int f) { (void)p; (void)f; return -1; }
static int  d_read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; return -1; }
static int  d_write(int fd, const void *b, size_t n) { (void)fd; (void)b; (void)n; return -1; }
static int  d_seek(int fd, int32_t o, int w) { (void)fd; (void)o; (void)w; return -1; }
static void d_close(int fd) { (void)fd; }
static int  d_list(const char *d, char *o, int m, int l) { (void)d; (void)o; (void)m; (void)l; return -1; }
static int  d_list_ex(const char *d, CappEntry *o, int m) { (void)d; (void)o; (void)m; return -1; }
static int  d_stat(const char *p, CappStat *s) { (void)p; (void)s; return -1; }
static int  d_path(const char *p) { (void)p; return -1; }
static int  d_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
static int  d_run(const char *n, const char *a) { (void)n; (void)a; return -1; }

static void *d_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *d_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *d_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static size_t d_strlen(const char *s) { return strlen(s); }
static int d_fmt(char *b, size_t n, const char *f, ...) {
  va_list ap;
  int r;
  va_start(ap, f);
  r = vsnprintf(b, n, f, ap);
  va_end(ap);
  return r;
}
static uint32_t d_ticks(void) { return fake_ticks; }
static void d_log(const char *m) { (void)m; }

static void d_out(const char *s) {
  size_t have = strlen(fake_out);
  snprintf(fake_out + have, sizeof fake_out - have, "%s", s);
}
static void d_out_line(const char *s) { d_out(s); d_out("\n"); }
static int  d_in_line(char *b, size_t n) { (void)b; (void)n; return -1; }
static int  d_zero(void) { return 0; }
static int  d_one(void) { return 1; }
static int  d_key_repeat(void) { return fake_repeat; }
static int  d_key_pending(void) { return fake_key_pending; }

static int d_http_get(const char *u, char *b, size_t n, int ms) { (void)u; (void)b; (void)n; (void)ms; return -1; }
static int d_net_connect(int ms) { (void)ms; return 0; }
static const char *d_net_status(void) { return "connected (fake)"; }
static int d_http_download(const char *u, const char *p, int ms) { (void)u; (void)p; (void)ms; return -1; }
static int d_http(const char *m, const char *u, const char *b, const char *ct, const char *br,
                  char *o, size_t n, int ms) {
  (void)m; (void)u; (void)b; (void)ct; (void)br; (void)ms;
  if (n) o[0] = 0;
  return -1;
}
static const char *d_null_str(void) { return NULL; }
static const char *d_empty_str(void) { return ""; }
static int d_caps(void) { return CAPP_CAP_NET | CAPP_CAP_PROXY; }
static int d_http_stream(const char *u, int (*cb)(void *, const uint8_t *, int), void *c, int ms) {
  (void)u; (void)cb; (void)c; (void)ms;
  return -1;
}
static void d_damage(CRect r) { (void)r; }
static CRect d_paint_area(void) { return fake_screen; }
static void d_ui(const CappUi *u) { (void)u; }
static int d_update(char *o, size_t n) { if (n) o[0] = 0; return 0; }
static int d_update_apply(int os, char *o, size_t n) { (void)os; if (n) o[0] = 0; return 0; }
static void d_now(CappTime *t) { *t = fake_now; }
static uint32_t d_epoch(void) { return fake_epoch; }
static void *d_exec_alloc(size_t n) { (void)n; return NULL; }
static void *d_exec_writable(void *p) { return p; }
static void d_exec_free(void *p) { (void)p; }
static int d_http_start(const char *m, const char *u, const char *b, const char *ct,
                        const char *br, int ms) {
  (void)m; (void)u; (void)b; (void)ct; (void)br; (void)ms;
  return -1;
}
static int d_http_poll(char *o, size_t n) { if (n) o[0] = 0; return -1; }

static int d_ag_ask(const char *t) { (void)t; return -4; }
static void d_void(void) {}
static unsigned d_gen(void) { return 0; }
static const CappAgent AGENT = { d_ag_ask, d_void, d_zero, d_zero, d_gen, d_empty_str, d_empty_str, d_void };
static const CappAgent *d_agent(void) { return &AGENT; }

static int  d_share_start(void) { return -1; }
static int  d_print(const char *doc) { (void)doc; return -2; }
static int  d_pick(const CappPick *p) { (void)p; return -1; }
static int  d_pick_poll(char *o, size_t n) { if (n) o[0] = 0; return 0; }

static int d_au_record(const char *p, int ms) { (void)p; (void)ms; return -2; }
static int d_au_play(const char *p) { (void)p; return -2; }
static int d_au_level(void) { return -1; }
static uint32_t d_u32(void) { return 0; }
static int d_au_last(void) { return -1; }
static void d_au_set_volume(int p) { (void)p; }
static int d_au_volume(void) { return 50; }
static void d_au_pause(int on) { (void)on; }
static void d_au_seek(uint32_t ms) { (void)ms; }
static const CappAudio AUDIO = { d_au_record, d_au_play, d_void, d_zero, d_au_level, d_u32, d_u32,
                                 d_au_last, d_empty_str, d_au_set_volume, d_au_volume,
                                 d_au_pause, d_zero, d_au_seek };
static const CappAudio *d_audio(void) { return &AUDIO; }

static const char *d_proxy(void) { return "http://srv"; }
static int  d_headless(void) { return fake_headless; }
static void d_command_done(int rc, const char *o) { (void)rc; (void)o; }
static int  d_font_load(const char *n) { (void)n; return -1; }
static void d_font_free(int f) { (void)f; }
static void d_text_font(int f, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  (void)f; (void)x; (void)y; (void)s; (void)fg; (void)bg;
}
static int d_text_width(int f, const char *s) { (void)f; return (int)strlen(s) * 6; }
static int d_font_height(int f) { (void)f; return 8; }
static int d_print_fonts(const char *d, const char *b, const char *bo, const char *h) {
  (void)d; (void)b; (void)bo; (void)h;
  return -2;
}
static void d_keep_awake(int on) { (void)on; }
static int d_run_command(const char *a, const char *l, char *o, size_t n) {
  (void)a; (void)l;
  if (n) o[0] = 0;
  return -1;
}
static int d_shell(const char *l, char *o, size_t n) { (void)l; if (n) o[0] = 0; return -1; }
static int d_http_upload(const char *u, const char *p, const char *ct, char *o, size_t n, int ms) {
  (void)u; (void)p; (void)ct; (void)ms;
  if (n) o[0] = 0;
  return -1;
}
static int d_motion(CappMotion *m) { (void)m; return -1; }

static int d_midi_open(int pin) { (void)pin; return -1; }
static int d_midi_send(const uint8_t *b, int n) { (void)b; return n; }
static int d_midi_play(const CappMidiEvent *e, int n, uint32_t l) { (void)e; (void)n; (void)l; return -1; }
static const CappMidi MIDI = { d_midi_open, d_void, d_midi_send, d_midi_play, d_void, d_zero, d_u32 };
static const CappMidi *d_midi(void) { return &MIDI; }

static int d_notify(const char *t, const char *x) { (void)t; (void)x; return 0; }
static int d_notify_at(uint32_t s, const char *k, const char *t, const char *x, int r) {
  (void)s; (void)k; (void)t; (void)x; (void)r;
  return 0;
}
static void d_notify_cancel(const char *k) { (void)k; }
static int d_update_progress(int os, void (*cb)(void *, const char *), void *c, char *o, size_t n) {
  (void)os; (void)cb; (void)c;
  if (n) o[0] = 0;
  return 0;
}
static int d_firmware_boot(const char *p, char *w, size_t n) {
  (void)p;
  if (n) snprintf(w, n, "not on the host");
  return -1;
}

static int d_link_open(const char *g, const char *m) { (void)g; (void)m; return -1; }
static const char *d_link_name(int i) { (void)i; return ""; }
static int d_link_int(int i) { (void)i; return -1; }
static int d_link_send(const void *b, int n) { (void)b; (void)n; return -1; }
static int d_link_recv(void *b, int n) { (void)b; (void)n; return 0; }
static const char *d_link_why(void) { return ""; }
static const CappLink LINK = { d_link_open, d_void, d_zero, d_zero, d_link_name, d_link_int,
                               d_link_int, d_zero, d_link_send, d_link_recv, d_void, d_link_why };
static const CappLink *d_link(void) { return &LINK; }

void fakeapi_init(CardApi *a) {
  memset(a, 0, sizeof *a);
  a->version = CAPP_API_VERSION;
  a->fill = d_fill; a->frame = d_frame; a->bevel = d_bevel; a->text = d_text; a->pixels = d_pixels;
  a->open = d_open; a->read = d_read; a->write = d_write; a->seek = d_seek; a->close = d_close;
  a->list = d_list; a->list_ex = d_list_ex; a->stat = d_stat;
  a->mkdir = d_path; a->remove = d_path; a->rename = d_rename; a->run = d_run;
  a->mem_set = d_memset; a->mem_cpy = d_memcpy; a->mem_move = d_memmove; a->str_len = d_strlen;
  a->fmt = d_fmt; a->ticks_ms = d_ticks; a->log = d_log;
  a->out = d_out; a->out_line = d_out_line; a->in_line = d_in_line; a->has_input = d_zero;
  a->http_get = d_http_get; a->net_ready = d_one; a->net_connect = d_net_connect;
  a->net_status = d_net_status; a->http_download = d_http_download; a->http = d_http;
  a->google_token = d_null_str; a->google_status = d_empty_str; a->caps_ok = d_caps;
  a->http_stream = d_http_stream; a->key_pending = d_zero;
  a->damage = d_damage; a->paint_area = d_paint_area; a->ui = d_ui;
  a->update_check = d_update; a->update_apply = d_update_apply;
  a->now = d_now; a->epoch = d_epoch;
  a->exec_alloc = d_exec_alloc; a->exec_writable = d_exec_writable; a->exec_free = d_exec_free;
  a->http_start = d_http_start; a->http_poll = d_http_poll;
  a->agent = d_agent;
  a->share_start = d_share_start; a->share_stop = d_void; a->share_status = d_empty_str;
  a->share_take_log = d_null_str;
  a->print = d_print; a->print_status = d_empty_str;
  a->key_repeat = d_zero;
  a->pick = d_pick; a->pick_poll = d_pick_poll;
  a->audio = d_audio; a->proxy = d_proxy;
  a->headless = d_headless; a->command_done = d_command_done;
  a->font_load = d_font_load; a->font_free = d_font_free; a->text_font = d_text_font;
  a->text_width = d_text_width; a->font_height = d_font_height; a->print_fonts = d_print_fonts;
  a->keep_awake = d_keep_awake; a->wake = d_void;
  a->run_command = d_run_command; a->shell = d_shell; a->http_upload = d_http_upload;
  a->motion = d_motion; a->midi = d_midi;
  a->notify = d_notify; a->notify_at = d_notify_at; a->notify_cancel = d_notify_cancel;
  a->update_apply_progress = d_update_progress; a->firmware_boot = d_firmware_boot;
  a->link = d_link;
  a->key_repeat = d_key_repeat;
  a->key_pending = d_key_pending;
  fake_out[0] = 0;
  fake_ticks = 1000;
  fake_repeat = fake_headless = fake_key_pending = 0;
  memset(&fake_now, 0, sizeof fake_now);
  fake_now.year = 2026; fake_now.month = 10; fake_now.day = 9;
  fake_now.hour = 12; fake_now.wday = 5; fake_now.synced = 2;
  fake_epoch = 1791547200u;                 /* 2026-10-09 12:00 UTC */
}

/* ---- the in-memory card ------------------------------------------------------ */

#define MF_MAX 256
#define MD_MAX 8

static struct { char path[128]; char *data; int len, cap, used, dir; } MF[MF_MAX];
static struct { int f, pos, write; } MD[MD_MAX];
int fakefs_writes, fakefs_removes;

static int mf_find(const char *p) {
  int i;
  for (i = 0; i < MF_MAX; i++) if (MF[i].used && !strcmp(MF[i].path, p)) return i;
  return -1;
}
static int mf_new(const char *p, int dir) {
  int i;
  for (i = 0; i < MF_MAX && MF[i].used; i++) {}
  if (i == MF_MAX) return -1;
  MF[i].used = 1; MF[i].dir = dir; MF[i].len = 0;
  snprintf(MF[i].path, sizeof MF[i].path, "%s", p);
  return i;
}
static void mf_room(int i, int need) {
  if (need + 1 <= MF[i].cap) return;
  MF[i].cap = (need + 1) * 2;
  MF[i].data = (char *)realloc(MF[i].data, (size_t)MF[i].cap);
}
/* Is `p` directly inside `dir`? */
static const char *mf_inside(const char *p, const char *dir) {
  size_t dl = strlen(dir);
  if (dl == 1 && dir[0] == '/') dl = 0;
  if (strncmp(p, dir, dl) || p[dl] != '/' || !p[dl + 1]) return NULL;
  if (strchr(p + dl + 1, '/')) return NULL;
  return p + dl + 1;
}
static int mf_is_dir(const char *p) {
  int i;
  size_t pl = strlen(p);
  if (!strcmp(p, "/")) return 1;
  for (i = 0; i < MF_MAX; i++) {
    if (!MF[i].used) continue;
    if (!strcmp(MF[i].path, p)) return MF[i].dir;
    if (!strncmp(MF[i].path, p, pl) && MF[i].path[pl] == '/') return 1;
  }
  return 0;
}

static int m_open(const char *p, int flags) {
  int i = mf_find(p), d;
  if (i >= 0 && MF[i].dir) return -1;
  if (i < 0) {
    if (!(flags & CAPP_O_WRITE)) return -1;
    if ((i = mf_new(p, 0)) < 0) return -1;
  }
  for (d = 0; d < MD_MAX && MD[d].f >= 0; d++) {}
  if (d == MD_MAX) return -1;
  if (flags & CAPP_O_WRITE) fakefs_writes++;
  if (flags & CAPP_O_TRUNC) MF[i].len = 0;
  MD[d].f = i; MD[d].pos = 0; MD[d].write = (flags & CAPP_O_WRITE) != 0;
  return d;
}
static int m_read(int d, void *b, size_t n) {
  int f, left;
  if (d < 0 || d >= MD_MAX || MD[d].f < 0) return -1;
  f = MD[d].f;
  left = MF[f].len - MD[d].pos;
  if ((int)n > left) n = (size_t)(left > 0 ? left : 0);
  if (n) memcpy(b, MF[f].data + MD[d].pos, n);
  MD[d].pos += (int)n;
  return (int)n;
}
static int m_write(int d, const void *b, size_t n) {
  int f;
  if (d < 0 || d >= MD_MAX || MD[d].f < 0 || !MD[d].write) return -1;
  f = MD[d].f;
  mf_room(f, MD[d].pos + (int)n);
  if (MD[d].pos > MF[f].len) memset(MF[f].data + MF[f].len, 0, (size_t)(MD[d].pos - MF[f].len));
  memcpy(MF[f].data + MD[d].pos, b, n);
  MD[d].pos += (int)n;
  if (MD[d].pos > MF[f].len) MF[f].len = MD[d].pos;
  return (int)n;
}
static int m_seek(int d, int32_t off, int whence) {
  int f, at;
  if (d < 0 || d >= MD_MAX || MD[d].f < 0) return -1;
  f = MD[d].f;
  at = whence == 0 ? off : whence == 1 ? MD[d].pos + off : MF[f].len + off;
  if (at < 0) return -1;
  MD[d].pos = at;
  return at;
}
static void m_close(int d) { if (d >= 0 && d < MD_MAX) MD[d].f = -1; }

static int m_list_ex(const char *dir, CappEntry *out, int max) {
  int i, j, n = 0;
  if (!mf_is_dir(dir)) return -1;
  for (i = 0; i < MF_MAX && n < max; i++) {
    const char *rest;
    char name[CAPP_NAME_MAX + 1];
    int is_dir;
    if (!MF[i].used) continue;
    /* A file deeper down makes the folder on the way an entry. */
    {
      size_t dl = strlen(dir);
      const char *p = MF[i].path, *slash;
      if (dl == 1 && dir[0] == '/') dl = 0;
      if (strncmp(p, dir, dl) || p[dl] != '/') continue;
      rest = p + dl + 1;
      slash = strchr(rest, '/');
      if (slash) {
        size_t k = (size_t)(slash - rest);
        if (k >= sizeof name) k = sizeof name - 1;
        memcpy(name, rest, k);
        name[k] = 0;
        is_dir = 1;
      } else {
        snprintf(name, sizeof name, "%s", rest);
        is_dir = MF[i].dir;
      }
    }
    for (j = 0; j < n; j++) if (!strcmp(out[j].name, name)) break;
    if (j < n) continue;
    memset(&out[n], 0, sizeof out[n]);
    snprintf(out[n].name, sizeof out[n].name, "%s", name);
    out[n].is_dir = is_dir;
    out[n].size = is_dir ? 0 : (uint32_t)MF[i].len;
    n++;
  }
  return n;
}
static int m_list(const char *dir, char *out, int max, int len) {
  static CappEntry e[MF_MAX];
  int n = m_list_ex(dir, e, max < MF_MAX ? max : MF_MAX), i;
  for (i = 0; i < n; i++) snprintf(out + (size_t)i * (size_t)len, (size_t)len, "%s", e[i].name);
  return n;
}
static int m_stat(const char *p, CappStat *st) {
  int i = mf_find(p);
  if (i < 0 && !mf_is_dir(p)) return -1;
  if (st) {
    memset(st, 0, sizeof *st);
    st->is_dir = i < 0 ? 1 : MF[i].dir;
    st->size = i < 0 || MF[i].dir ? 0 : (uint32_t)MF[i].len;
  }
  return 0;
}
static int m_mkdir(const char *p) {
  if (mf_find(p) >= 0) return mf_is_dir(p) ? 0 : -1;
  if (mf_is_dir(p)) return 0;
  return mf_new(p, 1) < 0 ? -1 : 0;
}
static int m_remove(const char *p) {
  int i = mf_find(p), k;
  size_t pl = strlen(p);
  for (k = 0; k < MF_MAX; k++)
    if (MF[k].used && !strncmp(MF[k].path, p, pl) && MF[k].path[pl] == '/') return -1;
  if (i < 0) return -1;
  MF[i].used = 0;
  MF[i].len = 0;
  fakefs_removes++;
  return 0;
}
static int m_rename(const char *a, const char *b) {
  int i = mf_find(a);
  if (i < 0 || mf_find(b) >= 0) return -1;
  snprintf(MF[i].path, sizeof MF[i].path, "%s", b);
  return 0;
}

void fakefs_mem(CardApi *a) {
  int i;
  for (i = 0; i < MF_MAX; i++) { MF[i].used = 0; MF[i].len = 0; }
  for (i = 0; i < MD_MAX; i++) MD[i].f = -1;
  fakefs_writes = fakefs_removes = 0;
  a->open = m_open; a->read = m_read; a->write = m_write; a->seek = m_seek; a->close = m_close;
  a->list = m_list; a->list_ex = m_list_ex; a->stat = m_stat;
  a->mkdir = m_mkdir; a->remove = m_remove; a->rename = m_rename;
}

void fakefs_put_bytes(const char *path, const void *data, int n) {
  int i = mf_find(path);
  if (i < 0) i = mf_new(path, 0);
  if (i < 0) return;
  mf_room(i, n);
  memcpy(MF[i].data, data, (size_t)n);
  MF[i].len = n;
}
void fakefs_put(const char *path, const char *text) { fakefs_put_bytes(path, text, (int)strlen(text)); }

const char *fakefs_get(const char *path) {
  int i = mf_find(path);
  if (i < 0 || MF[i].dir) return NULL;
  mf_room(i, MF[i].len);
  MF[i].data[MF[i].len] = 0;
  return MF[i].data;
}
int fakefs_size(const char *path) {
  int i = mf_find(path);
  return i < 0 || MF[i].dir ? -1 : MF[i].len;
}
int fakefs_exists(const char *path) { return mf_find(path) >= 0 || mf_is_dir(path); }
int fakefs_count(const char *dir) {
  static CappEntry e[MF_MAX];
  int n = m_list_ex(dir, e, MF_MAX);
  return n < 0 ? 0 : n;
}

/* ---- the card on the PC ------------------------------------------------------ */

#define HF_MAX 4
static FILE *HF[HF_MAX];
static char s_prefix[64];

void fakefs_host_name(const char *path, char *out, size_t n) {
  size_t j = 0, i;
  for (i = 0; s_prefix[i] && j + 1 < n; i++) out[j++] = s_prefix[i];
  for (i = 0; path[i] && j + 1 < n; i++) out[j++] = path[i] == '/' ? '_' : path[i];
  out[j] = 0;
}
static int h_open(const char *path, int flags) {
  char nm[160];
  int d;
  for (d = 0; d < HF_MAX && HF[d]; d++) {}
  if (d == HF_MAX) return -1;
  fakefs_host_name(path, nm, sizeof nm);
  if (flags & CAPP_O_WRITE) {
    fakefs_writes++;
    if (!(flags & CAPP_O_TRUNC)) {
      HF[d] = fopen(nm, "r+b");
      if (!HF[d]) HF[d] = fopen(nm, "w+b");
    } else {
      HF[d] = fopen(nm, "w+b");
    }
  } else {
    HF[d] = fopen(nm, "rb");
  }
  return HF[d] ? d : -1;
}
static int h_read(int d, void *b, size_t n) { return d >= 0 && d < HF_MAX && HF[d] ? (int)fread(b, 1, n, HF[d]) : -1; }
static int h_write(int d, const void *b, size_t n) { return d >= 0 && d < HF_MAX && HF[d] ? (int)fwrite(b, 1, n, HF[d]) : -1; }
static int h_seek(int d, int32_t off, int whence) {
  if (d < 0 || d >= HF_MAX || !HF[d]) return -1;
  if (fseek(HF[d], off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END)) return -1;
  return (int)ftell(HF[d]);
}
static void h_close(int d) { if (d >= 0 && d < HF_MAX && HF[d]) { fclose(HF[d]); HF[d] = NULL; } }
static int h_mkdir(const char *p) { (void)p; return 0; }
static int h_remove(const char *p) {
  char nm[160];
  fakefs_host_name(p, nm, sizeof nm);
  if (remove(nm) != 0) return -1;
  fakefs_removes++;
  return 0;
}
static int h_rename(const char *a, const char *b) {
  char x[160], y[160];
  FILE *t;
  fakefs_host_name(a, x, sizeof x);
  fakefs_host_name(b, y, sizeof y);
  if ((t = fopen(y, "rb")) != NULL) { fclose(t); return -1; }   /* the card's rule */
  return rename(x, y) == 0 ? 0 : -1;
}
static int h_stat(const char *p, CappStat *st) {
  char nm[160];
  FILE *t;
  long size;
  fakefs_host_name(p, nm, sizeof nm);
  if ((t = fopen(nm, "rb")) == NULL) return -1;
  fseek(t, 0, SEEK_END);
  size = ftell(t);
  fclose(t);
  if (st) { memset(st, 0, sizeof *st); st->size = (uint32_t)size; }
  return 0;
}
static int h_list_ex(const char *d, CappEntry *o, int m) { (void)d; (void)o; (void)m; return 0; }
static int h_list(const char *d, char *o, int m, int l) { (void)d; (void)o; (void)m; (void)l; return 0; }

void fakefs_host(CardApi *a, const char *prefix) {
  int i;
  for (i = 0; i < HF_MAX; i++) if (HF[i]) { fclose(HF[i]); HF[i] = NULL; }
  snprintf(s_prefix, sizeof s_prefix, "%s", prefix);
  fakefs_writes = fakefs_removes = 0;
  a->open = h_open; a->read = h_read; a->write = h_write; a->seek = h_seek; a->close = h_close;
  a->list = h_list; a->list_ex = h_list_ex; a->stat = h_stat;
  a->mkdir = h_mkdir; a->remove = h_remove; a->rename = h_rename;
}

void fakefs_host_clean(const char *path) {
  char nm[160], tmp[200];
  fakefs_host_name(path, nm, sizeof nm);
  remove(nm);
  snprintf(tmp, sizeof tmp, "%s.tmp", nm);
  remove(tmp);
}
