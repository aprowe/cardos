/* Hub: M5Launcher's firmware catalog (LauncherHub), browsed and booted.
 *
 * The server does the HTTPS and the JSON (server/m5hub.py) and answers in
 * lines; this pages through them 40 at a time. Enter on a firmware shows
 * its versions, Enter on a version downloads just its app image -- the
 * catalog says where it sits inside the whole-flash file -- to
 * /firmware/NAME.bin, and asks whether to boot it now: y hands it to the
 * same chain-boot the launcher's Firmware folder uses (api->firmware_boot),
 * one reset brings CardOS back. A firmware that also wants its own SPIFFS
 * or FAT partition is marked "+data": the app boots, but whatever it keeps
 * there will be missing.
 *
 * Keys: up/down move, left/right page, Enter open, s search (type, Enter),
 * Tab order (name, newest, starred), r reload, Esc back.
 */

#include "kernel/app/capp.h"
#include "apps/footer.h"
#include "apps/confirm.h"

static const CardApi *api;

#define PER       40
#define ROW_H     10
#define TOP_H     12
#define FW_DIR    "/firmware"

#define CLR_BG    CAPP_RGB(14, 16, 22)
#define CLR_BAR   CAPP_RGB(30, 34, 46)
#define CLR_TEXT  CAPP_RGB(236, 240, 248)
#define CLR_DIM   CAPP_RGB(128, 136, 150)
#define CLR_SEL   CAPP_RGB(44, 70, 110)
#define CLR_ACC   CAPP_RGB(96, 204, 196)
#define CLR_WARN  CAPP_RGB(255, 198, 96)
#define CLR_BAD   CAPP_RGB(228, 86, 76)

enum { V_LIST = 0, V_VERS, V_SEARCH, V_ASK, V_BOOTING };
enum { REQ_NONE = 0, REQ_LIST, REQ_INFO };
static const char *const ORDER[] = { "name", "date", "star" };
static const char *const ORDER_SAY[] = { "A-Z", "newest", "starred" };

typedef struct { char fid[33]; char name[41]; char author[17]; int star; } Item;
typedef struct { char ver[21]; char date[11]; char file[48]; uint32_t ao, as; int data; } Ver;

static struct {
  Item  it[PER];
  int   n, sel, top, page, pages, total, order;
  char  q[32], qedit[32];
  Ver   v[12];
  int   nv, vsel;
  char  title[41];
  int   view, req;
  char  msg[64];
  int   bad;                    /* msg is an error */
  char  saved[64];              /* the file just downloaded, for the boot question */
  int   fd, got, want, drawn, cancelled;
  CRect area;
  char  reply[6000];
} C;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static void say(int bad, const char *s) {
  api->fmt(C.msg, sizeof C.msg, "%s", s);
  C.bad = bad;
}

/* ---- parsing the server's lines ------------------------------------------ */

static char *field(char **p, char *out, size_t n) {
  size_t k = 0;
  while (**p && **p != '\t' && **p != '\n') {
    if (k + 1 < n) out[k++] = **p;
    (*p)++;
  }
  out[k] = 0;
  if (**p == '\t') (*p)++;
  return out;
}

static uint32_t num(const char *s) {
  uint32_t v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
  return v;
}

static char *next_line(char *p) {
  while (*p && *p != '\n') p++;
  return *p ? p + 1 : p;
}

static void parse_list(void) {
  char *p = C.reply, tmp[12];
  if (p[0] != 'o' || p[1] != 'k') { say(1, "the catalog did not answer"); return; }
  p += 3;
  C.page = (int)num(p); while (*p && *p != ' ') p++; if (*p) p++;
  C.pages = (int)num(p); while (*p && *p != ' ') p++; if (*p) p++;
  C.total = (int)num(p);
  p = next_line(p);
  C.n = 0;
  while (*p && C.n < PER) {
    Item *it = &C.it[C.n];
    field(&p, it->fid, sizeof it->fid);
    field(&p, it->name, sizeof it->name);
    field(&p, it->author, sizeof it->author);
    field(&p, tmp, sizeof tmp);
    it->star = (int)num(tmp);
    p = next_line(p);
    if (it->fid[0]) C.n++;
  }
  C.sel = C.top = 0;
  if (!C.n) say(0, C.q[0] ? "nothing matches" : "the catalog is empty");
  else C.msg[0] = 0;
}

static void parse_info(void) {
  char *p = C.reply, tmp[12];
  if (p[0] != 'o' || p[1] != 'k') { say(1, "the catalog did not answer"); C.view = V_LIST; return; }
  p = C.reply + 3;                          /* "ok NAME" */
  field(&p, C.title, sizeof C.title);
  p = next_line(p);
  C.nv = 0;
  while (*p && C.nv < 12) {
    Ver *v = &C.v[C.nv];
    field(&p, v->ver, sizeof v->ver);
    field(&p, v->date, sizeof v->date);
    field(&p, v->file, sizeof v->file);
    field(&p, tmp, sizeof tmp); v->ao = num(tmp);
    field(&p, tmp, sizeof tmp); v->as = num(tmp);
    field(&p, tmp, sizeof tmp); v->data = tmp[0] == '1';
    p = next_line(p);
    if (v->file[0] && v->as) C.nv++;
  }
  C.vsel = 0;
  C.msg[0] = 0;
  if (!C.nv) say(1, "no version of this can boot here");
}

/* ---- requests --------------------------------------------------------------- */

static void url_enc(char *out, size_t n, const char *s) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t k = 0;
  for (; *s && k + 4 < n; s++) {
    unsigned char ch = (unsigned char)*s;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
      out[k++] = (char)ch;
    else { out[k++] = '%'; out[k++] = HEX[ch >> 4]; out[k++] = HEX[ch & 15]; }
  }
  out[k] = 0;
}

static void ask_list(int page) {
  char url[200], q[100];
  url_enc(q, sizeof q, C.q);
  api->fmt(url, sizeof url, "%s/m5hub/list?page=%d&order=%s&q=%s", api->proxy(), page,
           ORDER[C.order], q);
  if (api->http_start("GET", url, 0, 0, "", 30000) == 0) { C.req = REQ_LIST; say(0, "asking the catalog..."); }
  else say(1, "busy: try again");
}

static void ask_info(void) {
  char url[160];
  if (!C.n) return;
  api->fmt(url, sizeof url, "%s/m5hub/info?fid=%s", api->proxy(), C.it[C.sel].fid);
  if (api->http_start("GET", url, 0, 0, "", 30000) == 0) {
    C.req = REQ_INFO;
    api->fmt(C.title, sizeof C.title, "%s", C.it[C.sel].name);
    C.nv = 0;
    C.view = V_VERS;
    say(0, "asking for its versions...");
  } else say(1, "busy: try again");
}

/* ---- downloading -------------------------------------------------------------- */

static void paint_progress(void) {
  char s[40];
  CRect r = rect(C.area.x, C.area.y + C.area.h - FOOT_H - 14, C.area.w, 12);
  /* in KB, 32-bit: an app has no 64-bit division */
  int w = C.want > 0 ? (int)((uint32_t)(C.got / 1024) * (uint32_t)(r.w - 16) /
                             (uint32_t)(C.want / 1024 + 1)) : 0;
  api->fill(rect(r.x + 8, r.y, r.w - 16, 4), CLR_BAR);
  api->fill(rect(r.x + 8, r.y, w, 4), CLR_ACC);
  api->fmt(s, sizeof s, "%d of %d KB  (any key stops)", C.got / 1024, C.want / 1024);
  api->text((int16_t)(r.x + 8), (int16_t)(r.y + 5), s, CLR_DIM, CLR_BG);
}

static int on_data(void *ctx, const uint8_t *d, int n) {
  (void)ctx;
  if (C.got + n > C.want || api->write(C.fd, d, (size_t)n) != n) return 1;
  C.got += n;
  if (C.got - C.drawn >= 32 * 1024 || C.got >= C.want) { paint_progress(); C.drawn = C.got; }
  if (api->key_pending()) { C.cancelled = 1; return 1; }
  return 0;
}

/* A file name from the firmware's name: lower case, letters and digits, at
 * most 20 of them, and the version's digits when there is room. */
static void file_name(char *out, size_t n, const char *name, const char *ver) {
  size_t k = 0;
  const char *s;
  for (s = name; *s && k < 20; s++) {
    char ch = *s;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) out[k++] = ch;
    else if (k && out[k - 1] != '-' && k < 19) out[k++] = '-';
  }
  while (k && out[k - 1] == '-') k--;
  if (!k) { out[k++] = 'f'; out[k++] = 'w'; }
  out[k] = 0;
  if (ver[0] && k < 18) {
    char v[12];
    size_t j = 0;
    for (s = ver; *s && j < 8; s++)
      if ((*s >= '0' && *s <= '9') || *s == '.') v[j++] = *s == '.' ? '_' : *s;
    v[j] = 0;
    if (j) api->fmt(out + k, n - k, "-%s", v);
  }
}

static void download(void) {
  const Ver *v = &C.v[C.vsel];
  char url[200], name[32], part[64];
  int r;
  if (!C.nv) return;
  file_name(name, sizeof name, C.title, v->ver);
  api->mkdir(FW_DIR);
  api->fmt(C.saved, sizeof C.saved, FW_DIR "/%s.bin", name);
  api->fmt(part, sizeof part, FW_DIR "/%s.part", name);
  api->fmt(url, sizeof url, "%s/m5hub/get?file=%s&ao=%lu&as=%lu", api->proxy(), v->file,
           (unsigned long)v->ao, (unsigned long)v->as);
  C.fd = api->open(part, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (C.fd < 0) { say(1, "could not write to the card"); return; }
  C.got = C.drawn = C.cancelled = 0;
  C.want = (int)v->as;
  paint_progress();
  r = api->http_stream(url, on_data, 0, 120000);
  api->close(C.fd);
  if (r < 0 || C.got != C.want) {
    api->remove(part);
    say(1, C.cancelled ? "stopped" : "the download did not come whole");
    return;
  }
  api->remove(C.saved);
  if (api->rename(part, C.saved) != 0) { say(1, "could not name the file"); return; }
  C.view = V_ASK;
  C.msg[0] = 0;
}

/* ---- painting ------------------------------------------------------------------ */

static void paint_top(CRect a, const char *left) {
  char right[32];
  api->fill(rect(a.x, a.y, a.w, TOP_H), CLR_BAR);
  api->text((int16_t)(a.x + 4), (int16_t)(a.y + 2), left, CLR_TEXT, CLR_BAR);
  if (C.view == V_LIST || C.view == V_SEARCH) {
    api->fmt(right, sizeof right, "%s %d/%d", ORDER_SAY[C.order], C.page, C.pages);
    api->text((int16_t)(a.x + a.w - 4 - (int)api->str_len(right) * 6), (int16_t)(a.y + 2),
              right, CLR_DIM, CLR_BAR);
  }
}

static void paint_rows(CRect a) {
  int rows = (a.h - TOP_H - FOOT_H - (C.msg[0] ? 10 : 0)) / ROW_H, i, y = a.y + TOP_H;
  if (C.sel < C.top) C.top = C.sel;
  if (C.sel >= C.top + rows) C.top = C.sel - rows + 1;
  for (i = C.top; i < C.n && i < C.top + rows; i++, y += ROW_H) {
    uint16_t bg = i == C.sel ? CLR_SEL : CLR_BG;
    char s[48];
    api->fill(rect(a.x, y, a.w, ROW_H), bg);
    api->fmt(s, sizeof s, "%s%.36s", C.it[i].star ? "* " : "", C.it[i].name);
    api->text((int16_t)(a.x + 4), (int16_t)(y + 1), s, C.it[i].star ? CLR_WARN : CLR_TEXT, bg);
  }
  if (y < a.y + a.h - FOOT_H) api->fill(rect(a.x, y, a.w, a.y + a.h - FOOT_H - y), CLR_BG);
}

static void paint_versions(CRect a) {
  int i, y = a.y + TOP_H + 2;
  api->fill(rect(a.x, a.y + TOP_H, a.w, a.h - TOP_H - FOOT_H), CLR_BG);
  if (C.n) {
    char s[48];
    api->fmt(s, sizeof s, "by %s", C.it[C.sel].author);
    api->text((int16_t)(a.x + 4), (int16_t)y, s, CLR_DIM, CLR_BG);
    y += 12;
  }
  for (i = 0; i < C.nv && y < a.y + a.h - FOOT_H - 22; i++, y += ROW_H) {
    uint16_t bg = i == C.vsel ? CLR_SEL : CLR_BG;
    char s[48];
    api->fill(rect(a.x, y, a.w, ROW_H), bg);
    api->fmt(s, sizeof s, "%-10.10s %s %4luK%s", C.v[i].ver, C.v[i].date,
             (unsigned long)(C.v[i].as / 1024), C.v[i].data ? " +data" : "");
    api->text((int16_t)(a.x + 4), (int16_t)(y + 1), s, C.v[i].data ? CLR_WARN : CLR_TEXT, bg);
  }
  if (C.nv && C.v[C.vsel].data) {
    api->text((int16_t)(a.x + 4), (int16_t)(a.y + a.h - FOOT_H - 22),
              "+data: wants its own storage too;", CLR_WARN, CLR_BG);
    api->text((int16_t)(a.x + 4), (int16_t)(a.y + a.h - FOOT_H - 12),
              "it boots, but may miss its files", CLR_WARN, CLR_BG);
  }
}

static void paint_center(CRect a, const char *l1, const char *l2, uint16_t c1) {
  api->fill(rect(a.x, a.y + TOP_H, a.w, a.h - TOP_H - FOOT_H), CLR_BG);
  api->text((int16_t)(a.x + (a.w - (int)api->str_len(l1) * 6) / 2), (int16_t)(a.y + 50), l1, c1, CLR_BG);
  if (l2) api->text((int16_t)(a.x + (a.w - (int)api->str_len(l2) * 6) / 2), (int16_t)(a.y + 64), l2, CLR_DIM, CLR_BG);
}

static void app_paint(void *st, CRect a) {
  char s[48];
  (void)st;
  C.area = a;
  switch (C.view) {
  case V_LIST:
    if (C.q[0]) api->fmt(s, sizeof s, "Hub: \"%.14s\" %d", C.q, C.total);
    else api->fmt(s, sizeof s, "Hub  %d firmware", C.total);
    paint_top(a, s);
    paint_rows(a);
    footer_paint(api, a, "enter open  s search  tab order  < > page");
    break;
  case V_SEARCH:
    paint_top(a, "Hub: search");
    paint_rows(a);
    api->fmt(s, sizeof s, "find: %s_", C.qedit);
    footer_paint(api, a, s);
    break;
  case V_VERS:
    api->fmt(s, sizeof s, "%.38s", C.title);
    paint_top(a, s);
    paint_versions(a);
    footer_paint(api, a, "enter download+boot  esc back");
    break;
  case V_ASK:
    paint_top(a, "Hub: downloaded");
    paint_center(a, C.saved, "boot it now? y/n  (reset comes home)", CLR_ACC);
    footer_paint(api, a, "y boot  n keep it in Firmware");
    break;
  case V_BOOTING:
    paint_top(a, "Hub");
    paint_center(a, "booting...", "copying it into the guest slot", CLR_TEXT);
    footer_paint(api, a, 0);
    break;
  }
  if (C.msg[0] && C.view != V_ASK && C.view != V_BOOTING)
    api->text((int16_t)(a.x + 4), (int16_t)(a.y + a.h - FOOT_H - 10), C.msg,
              C.bad ? CLR_BAD : CLR_DIM, CLR_BG);
}

/* ---- keys ------------------------------------------------------------------------ */

static int app_wants_text(void *st) { (void)st; return C.view == V_SEARCH; }

static int app_key(void *st, uint8_t k) {
  size_t n;
  (void)st;
  if (C.req != REQ_NONE && C.view != V_SEARCH && k != CAPP_KEY_ESC) return 1;
  switch (C.view) {
  case V_SEARCH:
    n = api->str_len(C.qedit);
    if (k == CAPP_KEY_ENTER) {
      api->fmt(C.q, sizeof C.q, "%s", C.qedit);
      C.view = V_LIST;
      ask_list(1);
    } else if (k == CAPP_KEY_ESC) C.view = V_LIST;
    else if (k == CAPP_KEY_BACK || k == 0x7F) { if (n) C.qedit[n - 1] = 0; }
    else if (k >= 32 && k < 127 && n + 1 < sizeof C.qedit) { C.qedit[n] = (char)k; C.qedit[n + 1] = 0; }
    return 1;
  case V_ASK:
    switch (confirm_key(api, k)) {
    case CONFIRM_YES: C.view = V_BOOTING; return 1;   /* tick boots, after the paint */
    case CONFIRM_NO:
      C.view = V_VERS;
      say(0, "kept: the launcher's Firmware folder boots it");
      return 1;
    default: return 1;
    }
  case V_BOOTING:
    return 1;
  case V_VERS:
    if (k == CAPP_KEY_ESC || k == CAPP_KEY_BACK) { C.view = V_LIST; C.msg[0] = 0; return 1; }
    if (k == CAPP_KEY_UP && C.vsel > 0) C.vsel--;
    else if (k == CAPP_KEY_DOWN && C.vsel < C.nv - 1) C.vsel++;
    else if (k == CAPP_KEY_ENTER) download();
    return 1;
  default:
    break;
  }
  switch (k) {
  case CAPP_KEY_UP:    if (C.sel > 0) C.sel--; return 1;
  case CAPP_KEY_DOWN:  if (C.sel < C.n - 1) C.sel++; return 1;
  case CAPP_KEY_LEFT:  if (C.page > 1) ask_list(C.page - 1); return 1;
  case CAPP_KEY_RIGHT: if (C.page < C.pages) ask_list(C.page + 1); return 1;
  case CAPP_KEY_ENTER: ask_info(); return 1;
  case '\t':           C.order = (C.order + 1) % 3; ask_list(1); return 1;
  case 's': case '/':  C.view = V_SEARCH; api->fmt(C.qedit, sizeof C.qedit, "%s", C.q); return 1;
  case 'r':            ask_list(C.page ? C.page : 1); return 1;
  case CAPP_KEY_ESC:
    if (C.q[0]) { C.q[0] = 0; ask_list(1); return 1; }   /* out of a search */
    return 0;
  default:             return 0;
  }
}

static int app_tick(void *st, uint32_t now) {
  int n;
  (void)st; (void)now;
  if (C.view == V_BOOTING) {
    char why[64];
    api->firmware_boot(C.saved, why, sizeof why);   /* returns only if it failed */
    C.view = V_VERS;
    say(1, why);
    return 1;
  }
  if (C.req == REQ_NONE) return 0;
  n = api->http_poll(C.reply, sizeof C.reply - 1);
  if (n == CAPP_HTTP_PENDING) return 0;
  if (n < 0) {
    say(1, n == -403 ? "the server refused: is this device signed in?" : "the server did not answer");
    if (C.req == REQ_INFO) C.view = V_LIST;
    C.req = REQ_NONE;
    return 1;
  }
  C.reply[n] = 0;
  if (C.req == REQ_LIST) parse_list(); else parse_info();
  C.req = REQ_NONE;
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Hub",
  /* 16x16: a chip with a down arrow into it. */
  { 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x07, 0xE0,
    0x03, 0xC0, 0x01, 0x80, 0x00, 0x00, 0x3F, 0xFC,
    0x20, 0x04, 0x2F, 0xF4, 0x28, 0x14, 0x28, 0x14,
    0x2F, 0xF4, 0x20, 0x04, 0x3F, 0xFC, 0x00, 0x00 },
  "up/down\tmove\nleft/right\tpage\nenter\tversions; on a version, download\n"
  "s\tsearch (type, enter)\ntab\torder: A-Z, newest, starred\nr\treload\n"
  "esc\tback, or out of a search\ny/n\tafter a download: boot it now\n",
  0,
  0,
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&C, 0, sizeof C);
  C.page = C.pages = 1;
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  ask_list(1);
  return 0;
}
