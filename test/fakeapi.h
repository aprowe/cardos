/* A CardApi for host tests: every entry filled with something harmless, so
 * a test scripts only what its app's behaviour depends on.
 *
 * Before this, each app test built its own table from scratch and set only
 * the fields its app happened to call -- the same fmt, mem_* and host-file
 * card written out eighteen times under different prefixes -- and a new
 * API entry an app started using crashed whichever test had not added it.
 *
 *   static CardApi A;
 *   fakeapi_init(&A);              every entry, a default (below)
 *   fakefs_mem(&A);                and an in-memory card, or
 *   fakefs_host(&A, "todo_test");  a card made of files on the PC
 *   A.http = my_http;              then what the test is about
 *   api = &A;
 *
 * The defaults: drawing does nothing; fmt is vsnprintf; the clock is
 * fakeapi_now / fakeapi_epoch / fakeapi_ticks, which a test may set; the network is
 * up and every request fails with -1; paint_area is FAKE_SCREEN; fonts are
 * the 6x8 one; key_repeat, headless and key_pending read fakeapi_repeat,
 * fakeapi_headless and fakeapi_key_pending; out and out_line append to fakeapi_out;
 * proxy is "http://srv"; audio, midi, link and agent are tables that do
 * nothing; anything that would start something else returns -1;
 * sig_verify answers -1, could not check.
 *
 * fakeapi.c is not a test file (gen_test_main.py scans test_*.c only); the
 * host build compiles it with everything else under test/.
 */
#ifndef CARDOS_FAKEAPI_H
#define CARDOS_FAKEAPI_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/app/capp.h"

void fakeapi_init(CardApi *a);

extern CappTime fakeapi_now;          /* what now() fills in */
extern uint32_t fakeapi_epoch;        /* what epoch() returns */
extern uint32_t fakeapi_ticks;        /* what ticks_ms() returns */
extern int      fakeapi_repeat;       /* what key_repeat() returns */
extern int      fakeapi_headless;     /* what headless() returns */
extern int      fakeapi_key_pending;  /* what key_pending() returns */
extern int      fakeapi_paint_direct; /* the last paint_direct(on); -1 never called */
extern char     fakeapi_out[4096];    /* what out() and out_line() wrote */
extern CRect    fakeapi_screen;       /* what paint_area() returns: 240x135 */

/* ---- an in-memory card ----
 *
 * Files of any size, folders made by mkdir or by a file inside them. The
 * card's rules: rename will not replace a file that is there, and remove
 * will not remove a folder with anything in it. Reset by fakefs_mem. */
void        fakefs_mem(CardApi *a);
void        fakefs_put(const char *path, const char *text);
void        fakefs_put_bytes(const char *path, const void *data, int n);
const char *fakefs_get(const char *path);     /* NUL-terminated, or NULL */
int         fakefs_size(const char *path);    /* bytes, or -1 */
int         fakefs_exists(const char *path);
int         fakefs_count(const char *dir);    /* entries directly inside */
extern int  fakefs_writes;                    /* opens for writing so far */
extern int  fakefs_removes;                   /* files removed so far */

/* ---- a card on the PC ----
 *
 * Each path is a file in the current directory named `prefix` plus the path
 * with its slashes as underscores, which is what the app tests did before.
 * No folders: mkdir succeeds and lists are empty. fakefs_host_clean removes
 * the file for one path, and its .tmp. */
void fakefs_host(CardApi *a, const char *prefix);
void fakefs_host_name(const char *path, char *out, size_t n);
void fakefs_host_clean(const char *path);

#endif /* CARDOS_FAKEAPI_H */
