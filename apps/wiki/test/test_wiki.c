/* Wikipedia on the host: the server's lines read back, the wrapping, paging
 * across the server's pages, the keys' levels, what is printed and what
 * the commands say. The server's half, against a fake Wikipedia, is
 * apps/wiki/server/tests/test_wiki.py. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define capp_info wiki_capp_info
#define capp_main wiki_capp_main
#include "apps/wiki/wiki.c"
#undef capp_info
#undef capp_main

static char s_url[400];
static const char *s_reply;
static int s_status;                  /* 0 answer s_reply, else this (negative) */
static int s_pending, s_started;

static int t_start(const char *m, const char *url, const char *body, const char *ct,
                   const char *bearer, int ms) {
  (void)m; (void)body; (void)ct; (void)bearer; (void)ms;
  if (s_pending) return -1;
  snprintf(s_url, sizeof s_url, "%s", url);
  s_pending = 1;
  s_started++;
  return 0;
}
static int t_poll(char *out, size_t n) {
  if (!s_pending) return -1;
  s_pending = 0;
  if (s_status) { snprintf(out, n, "%s", s_reply ? s_reply : ""); return s_status; }
  snprintf(out, n, "%s", s_reply);
  return (int)strlen(out);
}
static int t_http(const char *m, const char *url, const char *body, const char *ct,
                  const char *bearer, char *out, size_t n, int ms) {
  (void)m; (void)body; (void)ct; (void)bearer; (void)ms;
  snprintf(s_url, sizeof s_url, "%s", url);
  snprintf(out, n, "%s", s_reply);
  return s_status ? s_status : (int)strlen(out);
}

static char s_doc[8192];
static int t_print_fonts(const char *doc, const char *b, const char *bo, const char *h) {
  (void)b; (void)bo; (void)h;
  snprintf(s_doc, sizeof s_doc, "%s", doc);
  return 0;
}
static const char *t_print_status(void) { return "printing 10%"; }

static CardApi TF;

static void topen(void) {
  fakeapi_init(&TF);
  TF.http_start = t_start; TF.http_poll = t_poll; TF.http = t_http;
  TF.print_fonts = t_print_fonts; TF.print_status = t_print_status;
  api = &TF;
  memset(&G, 0, sizeof G);
  G.f_ui = G.f_uib = -1;
  measure_fonts();                      /* 6 px a character, as the 6x8 font */
  G.content = capp_rect(0, 0, 240, 135);
  s_pending = s_started = s_status = 0;
  s_url[0] = s_doc[0] = 0;
  s_reply = "";
}

/* Answer the request that is out, and let tick take it. */
static void answer(const char *reply) {
  s_reply = reply;
  app_tick(0, 0);
}

static void type(const char *s) { while (*s) app_key(0, (uint8_t)*s++); }

static const char *line_text(int i, char *buf, int n) {
  int k = G.line[i].len < n - 1 ? G.line[i].len : n - 1;
  memcpy(buf, G.art + G.line[i].off, (size_t)k);
  buf[k] = 0;
  return buf;
}

void test_wiki_search_is_typed_and_sent_encoded(void) {
  topen();
  CHECK(app_wants_text(0));
  type("ada & co");
  app_key(0, CAPP_KEY_ENTER);
  CHECK(!strcmp(s_url, "http://srv/wiki/search?q=ada%20%26%20co"));
  answer("Ada Lovelace\tEnglish mathematician\nAda (language)\tProgramming language\n");
  CHECK_EQ(G.view, V_RESULTS);
  CHECK(!app_wants_text(0));
  CHECK_EQ(G.nres, 2);
  CHECK(!strcmp(G.res[0].title, "Ada Lovelace") && !strcmp(G.res[0].desc, "English mathematician"));
  CHECK(!strcmp(G.res[1].title, "Ada (language)"));
  CHECK(!strcmp(G.searched, "ada & co"));
}

void test_wiki_nothing_found_says_so(void) {
  topen();
  type("zzqx");
  app_key(0, CAPP_KEY_ENTER);
  answer("");
  CHECK_EQ(G.view, V_RESULTS);
  CHECK_EQ(G.nres, 0);
  CHECK(!strcmp(G.status, "nothing found"));
}

void test_wiki_enter_with_nothing_typed_is_a_random_article(void) {
  topen();
  app_key(0, CAPP_KEY_ENTER);
  CHECK(!strcmp(s_url, "http://srv/wiki/random"));
  answer("page\t1\t1\tPoecilia vandepolli\nA fish.\n");
  CHECK_EQ(G.view, V_ARTICLE);
  CHECK(!strcmp(G.title, "Poecilia vandepolli"));
}

void test_wiki_an_article_opens_from_the_results(void) {
  topen();
  type("moon");
  app_key(0, CAPP_KEY_ENTER);
  answer("Moon\tSatellite\nMoon landing\tArrival\n");
  app_key(0, CAPP_KEY_DOWN);
  app_key(0, CAPP_KEY_ENTER);
  CHECK(!strcmp(s_url, "http://srv/wiki/page?page=1&title=Moon%20landing"));
  answer("page\t1\t3\tMoon landing\nA Moon landing is the arrival.\n## History\nIt began.\n");
  CHECK_EQ(G.view, V_ARTICLE);
  CHECK(G.page == 1 && G.pages == 3);
  /* Esc goes back a level at a time, and declines at the top. */
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);
  CHECK_EQ(G.view, V_RESULTS);
  CHECK_EQ(G.rsel, 1);
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);
  CHECK_EQ(G.view, V_SEARCH);
  CHECK(!strcmp(G.query, "moon"));
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);       /* clears what was typed */
  CHECK_EQ(G.qlen, 0);
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);       /* back to the results there were */
  CHECK_EQ(G.view, V_RESULTS);
  app_key(0, CAPP_KEY_ESC);
  G.nres = 0;
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 0);
}

/* Paragraphs wrap at spaces to the width, headings lose their marks and are
 * set apart, and a word wider than the screen is broken. 240 - 12 is 228
 * pixels: 38 characters of the 6x8 font. */
void test_wiki_the_text_wraps_at_words(void) {
  char buf[200];
  int i;
  topen();
  G.page = 1; G.pages = 1;
  snprintf(G.reply, sizeof G.reply, "page\t1\t1\tT\n%s\n## Early life\n%s\n### Sub\nx\n",
           "The quick brown fox jumps over the lazy dog and then keeps on running far away.",
           "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghij");
  CHECK_EQ(absorb_page(), 0);
  for (i = 0; i < G.nlines; i++)
    CHECK(G.line[i].kind == L_GAP || G.line[i].len * 6 <= 228);
  CHECK(!strcmp(line_text(0, buf, sizeof buf), "The quick brown fox jumps over the"));
  CHECK(!strcmp(line_text(1, buf, sizeof buf), "lazy dog and then keeps on running far"));
  CHECK(!strcmp(line_text(2, buf, sizeof buf), "away."));
  CHECK_EQ(G.line[3].kind, L_GAP);
  CHECK_EQ(G.line[4].kind, L_GAP);              /* a heading has room above it */
  CHECK_EQ(G.line[5].kind, L_H2);
  CHECK(!strcmp(line_text(5, buf, sizeof buf), "Early life"));
  CHECK_EQ(G.line[7].kind, L_BODY);
  CHECK_EQ(G.line[7].len, 38);                  /* the long word, broken */
  CHECK_EQ(G.line[8].len, 12);
  CHECK_EQ(G.line[11].kind, L_H3);
  CHECK(!strcmp(line_text(11, buf, sizeof buf), "Sub"));
  CHECK_EQ(G.line[G.nlines - 1].kind, L_BODY);  /* no gap at the end */
}

static char s_long[6000];

static const char *long_page(int n, int of) {
  int o = snprintf(s_long, sizeof s_long, "page\t%d\t%d\tLong\n", n, of), i;
  for (i = 0; i < 40; i++) o += snprintf(s_long + o, sizeof s_long - o, "Page %d paragraph %d.\n", n, i);
  return s_long;
}

/* Scrolling past the end of a page asks for the next; up from the top of
 * one asks for the one before and lands at its end. */
void test_wiki_scrolling_crosses_the_servers_pages(void) {
  int i, guard;
  topen();
  type("long");
  app_key(0, CAPP_KEY_ENTER);
  answer("Long\t\n");
  app_key(0, CAPP_KEY_ENTER);
  answer(long_page(1, 2));
  CHECK_EQ(G.top, 0);
  CHECK(G.nlines > fits_from(0));
  app_key(0, CAPP_KEY_DOWN);
  CHECK_EQ(G.top, 1);
  app_key(0, CAPP_KEY_UP);
  CHECK_EQ(G.top, 0);
  s_started = 0;
  app_key(0, CAPP_KEY_UP);                      /* page 1's top: nothing before */
  CHECK_EQ(s_started, 0);
  for (guard = 0; guard < 100 && !s_started; guard++) app_key(0, ' ');
  CHECK_EQ(s_started, 1);
  CHECK(!strcmp(s_url, "http://srv/wiki/page?page=2&title=Long"));
  answer(long_page(2, 2));
  CHECK(G.page == 2 && G.top == 0);
  /* The end of the last page goes no further. */
  for (i = 0; i < 100; i++) app_key(0, CAPP_KEY_DOWN);
  CHECK_EQ(s_started, 1);
  CHECK(G.top + fits_from(G.top) >= G.nlines);
  /* Back up past the top of page 2: page 1, at its end. */
  for (i = 0; i < 200 && s_started == 1; i++) app_key(0, CAPP_KEY_UP);
  CHECK_EQ(s_started, 2);
  CHECK(!strcmp(s_url, "http://srv/wiki/page?page=1&title=Long"));
  answer(long_page(1, 2));
  CHECK_EQ(G.page, 1);
  CHECK(G.top > 0);
  CHECK_EQ(G.top + fits_from(G.top), G.nlines);
  /* A screen up from there, then down again, comes back to the same place. */
  i = G.top;
  app_key(0, CAPP_KEY_LEFT);
  CHECK(G.top < i);
  app_key(0, CAPP_KEY_RIGHT);
  CHECK(G.top >= i - 1 && G.top <= i + 1);
}

void test_wiki_a_failed_page_keeps_the_one_on_screen(void) {
  topen();
  app_key(0, CAPP_KEY_ENTER);
  answer(long_page(1, 2));
  app_key(0, CAPP_KEY_DOWN);
  s_status = -404;
  for (int i = 0; i < 100 && !s_pending; i++) app_key(0, ' ');
  answer("error Long has 1 pages\n");
  CHECK_EQ(G.view, V_ARTICLE);
  CHECK_EQ(G.page, 1);
  CHECK(!strcmp(G.status, "Long has 1 pages") && G.bad);
}

void test_wiki_print_is_the_page_with_its_title(void) {
  topen();
  app_key(0, CAPP_KEY_ENTER);
  answer("page\t2\t3\tMoon\nIt orbits.\n## Name\nMoon.\n### Size\nBig.\n");
  CHECK_EQ(app_action(0, ACT_PRINT), 1);
  CHECK(!strcmp(s_doc, "# Moon\n(page 2 of 3)\nIt orbits.\n## Name\nMoon.\n## Size\nBig.\n"));
  CHECK_EQ(G.printing, 1);
  app_tick(0, 0);
  CHECK(!strcmp(G.status, "printing 10%"));
  /* Not from the search box. */
  topen();
  s_doc[0] = 0;
  app_action(0, ACT_PRINT);
  CHECK(!s_doc[0]);
  CHECK(!strcmp(G.status, "open an article to print it"));
}

void test_wiki_summary_is_the_first_paragraph(void) {
  const char *argv[1] = { "the moon" };
  char out[256];
  topen();
  s_reply = "Moon\tNatural satellite\nThe Moon is Earth's only natural satellite.\n";
  CHECK_EQ(app_command(0, ACT_SUMMARY, 1, argv, out, sizeof out), 0);
  CHECK(!strcmp(s_url, "http://srv/wiki/summary?q=the%20moon"));
  CHECK(!strcmp(out, "The Moon is Earth's only natural satellite."));
  /* Too long for out: cut at a word. */
  CHECK_EQ(app_command(0, ACT_SUMMARY, 1, argv, out, 24), 0);
  CHECK(!strcmp(out, "The Moon is Earth's..."));
  s_status = -404;
  s_reply = "error nothing on Wikipedia about the moon\n";
  CHECK_EQ(app_command(0, ACT_SUMMARY, 1, argv, out, sizeof out), -1);
  CHECK(!strcmp(out, "nothing on Wikipedia about the moon"));
}

void test_wiki_featured_gives_the_title_then_the_paragraph(void) {
  char out[256];
  topen();
  s_reply = "Moon\tNatural satellite\nThe Moon - featured.\n";
  CHECK_EQ(app_command(0, ACT_FEATURED, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(s_url, "http://srv/wiki/featured"));
  CHECK(!strcmp(out, "Moon\nThe Moon - featured."));
}

void test_wiki_open_searches_for_its_words(void) {
  char a0[] = "wiki", a1[] = "ada", a2[] = "lovelace";
  char *argv[3] = { a0, a1, a2 };
  topen();
  fakeapi_headless = 0;
  wiki_capp_main(&TF, 3, argv);
  CHECK(!strcmp(G.query, "ada lovelace"));
  CHECK(!strcmp(s_url, "http://srv/wiki/search?q=ada%20lovelace"));
}
