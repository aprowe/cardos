# WebDAV Share Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `share` in the console, or the Share app in the launcher, puts the SD card on the LAN as a WebDAV server that Windows Explorer and Finder mount as a drive.

**Architecture:** `kernel/net/dav.c` is portable C (request parsing, path coding, dates, XML) exercised by the host test suite; `kernel/net/share.c` is the device-only glue — one FreeRTOS task, one lwIP listening socket, one connection at a time, files through `fs.h`. Four new entries in the app API (version 23) let `apps/share.c` be a hundred lines of UI. `fs_fat.c` gets a lock around its open-file table and a per-handle directory prefix so a second task can use the card.

**Tech Stack:** C11, ESP-IDF (lwIP BSD sockets, FreeRTOS, FatFs via VFS), host tests via `host/CMakeLists.txt` + `test/tinytest.h`, Python 3 for the device-side client test.

**Spec:** `docs/superpowers/specs/2026-09-14-webdav-share-design.md`

## Global Constraints

- Port **80**; root is the whole card at `/`; **no authentication**.
- **One connection at a time**, keep-alive, listen backlog **4**, 5 s idle timeout on a kept-alive connection.
- Task: priority 3, core 1, **8 KB** stack, created on start and deleted on stop. It **never touches the display** (the `bg.c` rule) — it posts log lines to a queue.
- `share_start` refuses below **40 KB** free heap, with a sentence saying so.
- Transfers through one **4 KB** buffer; request line + headers limited to **2 KB** (over → 431, close).
- `OPTIONS` advertises `DAV: 1,2` and `MS-Author-Via: DAV`. `LOCK` always succeeds with an opaque token; `PROPPATCH` answers 207 with 200 and changes nothing. `Depth: infinity` → 403. `Range` ignored. Chunked `PUT` → 411. Folder `COPY` → 403.
- Every traversal shape (`..`, `%2e%2e`, backslashes, empty segments) is rejected with 403 before any file call.
- Host suite: `build\hostbuild.bat` from the worktree root (regenerates `test/test_main.c`, builds with MSVC/Ninja, runs). Baseline: 316 tests, 12431 checks, 0 failures. Every task must leave it at 0 failures.
- Firmware: `python tools/build_apps.py` then `python -m platformio run` (add `-t upload --upload-port COM3` to flash). Any change to `apps/` or `kernel/app/capp.h` needs `build_apps.py` first.
- Commit after every task with the attribution lines from the session (`Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01ABbRyWJi63YLndyN49cyfB`). Use `rtk git ...`.
- MSVC builds the host suite at `/W4` with implicit declarations as errors; no VLAs, no `//` comment style differences matter, but every function used must be declared.

---

### Task 1: The card from two tasks — `fs.h` gains `mtime`, a per-handle dir prefix, and a lock on the open table

**Files:**
- Modify: `kernel/fs/fs.h:27-33` (FsStat), `:35-39` (FsEntry), `:71` (FsDir)
- Modify: `kernel/fs/fs_fat.c:35-40` (statics), `:157-175` (alloc_fd/fs_open), `:192-197` (fs_close), `:200-244` (stat/opendir/readdir)
- Modify: `kernel/app/cardapi.c:91-112` only if it stops compiling (it copies `name/size/is_dir`; adding fields does not break it)

**Interfaces:**
- Produces: `FsStat { uint32_t size; int is_dir; uint32_t mtime; }`, `FsEntry { char name[64]; uint32_t size; int is_dir; uint32_t mtime; }`, `FsDir { void *impl; char prefix[FS_PATH_MAX + 8]; }`. `mtime` is seconds since the epoch as the VFS reports it, 0 if unknown. Everything else in `fs.h` unchanged.

There is no host test for this file (it is device-only); verification is that the firmware builds and `ls` on the device still lists `/desktop`.

- [ ] **Step 1: Add the fields in `fs.h`**

```c
typedef struct {
  uint32_t size;
  int      is_dir;
  uint32_t mtime;      /* seconds since the epoch, 0 if the card does not say */
} FsStat;

typedef struct {
  char     name[FS_NAME_MAX + 1];
  uint32_t size;
  int      is_dir;
  uint32_t mtime;
} FsEntry;
```

and replace the `FsDir` typedef with:

```c
/* The directory being read, and where it is, so that two tasks can each be
 * in the middle of a listing. Was a static, which meant one listing at a
 * time for the whole machine. */
typedef struct {
  void *impl;
  char  prefix[FS_PATH_MAX + 8];
} FsDir;
```

- [ ] **Step 2: Lock the open table in `fs_fat.c`**

Add includes and a spinlock next to the statics:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The share server opens files from its own task while the shell may be
 * opening one too. FatFs itself is built reentrant; this table was the only
 * unguarded thing. A critical section rather than a mutex because a slot
 * claim is eight pointer reads. */
static portMUX_TYPE  s_open_lock = portMUX_INITIALIZER_UNLOCKED;
```

Delete the `s_dir_prefix` static. Change `alloc_fd` to claim under the lock, using a sentinel so the slot is taken before `fopen` returns:

```c
#define FD_CLAIMED ((FILE *)1)

static int alloc_fd(void) {
  int i, fd = -1;
  taskENTER_CRITICAL(&s_open_lock);
  for (i = 0; i < FS_MAX_OPEN; i++)
    if (!s_open[i]) { s_open[i] = FD_CLAIMED; fd = i; break; }
  taskEXIT_CRITICAL(&s_open_lock);
  return fd;
}

static void free_fd(int fd) {
  taskENTER_CRITICAL(&s_open_lock);
  s_open[fd] = NULL;
  taskEXIT_CRITICAL(&s_open_lock);
}
```

In `fs_open`, after `f = fopen(real, mode);` replace `if (!f) return -1;` with `if (!f) { free_fd(fd); return -1; }`. In `file_of`, treat the sentinel as absent: `if (fd < 0 || fd >= FS_MAX_OPEN || s_open[fd] == FD_CLAIMED) return NULL; return s_open[fd];`. In `fs_close`, replace `s_open[fd] = NULL;` with `free_fd(fd);`.

- [ ] **Step 3: Fill `mtime` and move the prefix into `FsDir`**

`fs_stat`: add `out->mtime = (uint32_t)st.st_mtime;` after `is_dir`.

`fs_opendir`: replace the two `s_dir_prefix` lines with

```c
  strncpy(d->prefix, real, sizeof d->prefix - 1);
  d->prefix[sizeof d->prefix - 1] = '\0';
```

`fs_readdir`: use `d->prefix` in the `snprintf`, set `out->mtime = 0;` with the other zeroes, and inside the `stat(child, &st) == 0` block add `out->mtime = (uint32_t)st.st_mtime;`.

- [ ] **Step 4: Build the firmware**

Run: `python tools/build_apps.py` then `python -m platformio run`
Expected: build succeeds; no new warnings from `fs_fat.c`. (The host suite does not compile this file; run `build\hostbuild.bat` anyway to confirm nothing else was disturbed — expect 12431 checks, 0 failures.)

- [ ] **Step 5: Commit**

```bash
rtk git add kernel/fs/fs.h kernel/fs/fs_fat.c
rtk git commit -m "fs: mtime, a per-handle directory prefix, and a lock on the open table

So a second task can use the card. FatFs is already reentrant; the open
table and the static readdir prefix were the two things that were not."
```

---

### Task 2: `dav.c` — reading a request

**Files:**
- Create: `kernel/net/dav.h`, `kernel/net/dav.c`
- Create: `test/test_dav.c`
- Modify: `host/CMakeLists.txt:20` — add `${ROOT}/kernel/net/dav.c` after `manifest.c`

**Interfaces:**
- Produces:

```c
typedef enum {
  DAV_UNKNOWN = 0, DAV_OPTIONS, DAV_GET, DAV_HEAD, DAV_PUT, DAV_DELETE,
  DAV_MKCOL, DAV_PROPFIND, DAV_PROPPATCH, DAV_MOVE, DAV_COPY, DAV_LOCK,
  DAV_UNLOCK
} DavMethod;

#define DAV_DEPTH_INFINITY (-1)

typedef struct {
  DavMethod method;
  char     path[FS_PATH_MAX];   /* decoded, normalised CardOS path */
  char     dest[FS_PATH_MAX];   /* Destination, same treatment, or "" */
  int      depth;               /* 0, 1, or DAV_DEPTH_INFINITY (the default) */
  uint32_t content_length;
  int      has_content_length;
  int      chunked;             /* Transfer-Encoding: chunked */
  int      expect_continue;     /* Expect: 100-continue */
  int      keep_alive;          /* 1 unless Connection: close */
  int      overwrite;           /* 1 unless Overwrite: F */
  int      bad;                 /* 0, or the status to answer with instead */
} DavRequest;

int dav_headers_end(const char *buf, size_t n);          /* index past \r\n\r\n, or -1 */
int dav_parse(const char *hdr, size_t n, DavRequest *req); /* 0, or -1 with req->bad set */
const char *dav_method_name(DavMethod m);
```

- [ ] **Step 1: Write the failing tests**

`test/test_dav.c`:

```c
/* WebDAV on the host: everything in kernel/net/dav.c that is not a socket. */

#include <string.h>

#include "tinytest.h"
#include "kernel/net/dav.h"

void test_dav_finds_the_end_of_the_headers(void) {
  const char *r = "GET / HTTP/1.1\r\nHost: x\r\n\r\nbody";
  CHECK_EQ(dav_headers_end(r, strlen(r)), 27);
  CHECK_EQ(dav_headers_end(r, 10), -1);
  CHECK_EQ(dav_headers_end("", 0), -1);
}

void test_dav_parses_a_plain_get(void) {
  const char *r = "GET /desktop/a.txt HTTP/1.1\r\nHost: 1.2.3.4\r\n\r\n";
  DavRequest q;
  CHECK_EQ(dav_parse(r, strlen(r), &q), 0);
  CHECK_EQ(q.method, DAV_GET);
  CHECK(strcmp(q.path, "/desktop/a.txt") == 0);
  CHECK_EQ(q.keep_alive, 1);
  CHECK_EQ(q.depth, DAV_DEPTH_INFINITY);
  CHECK_EQ(q.has_content_length, 0);
  CHECK_EQ(q.overwrite, 1);
  CHECK_EQ(q.bad, 0);
}

void test_dav_reads_the_headers_it_cares_about(void) {
  const char *r =
    "PUT /x.bin HTTP/1.1\r\n"
    "content-length: 4096\r\n"
    "Connection: close\r\n"
    "Expect: 100-continue\r\n"
    "Depth: 1\r\n"
    "Overwrite: F\r\n"
    "Destination: http://1.2.3.4/y%20z.bin\r\n"
    "\r\n";
  DavRequest q;
  CHECK_EQ(dav_parse(r, strlen(r), &q), 0);
  CHECK_EQ(q.method, DAV_PUT);
  CHECK_EQ((int)q.content_length, 4096);
  CHECK_EQ(q.has_content_length, 1);
  CHECK_EQ(q.keep_alive, 0);
  CHECK_EQ(q.expect_continue, 1);
  CHECK_EQ(q.depth, 1);
  CHECK_EQ(q.overwrite, 0);
  CHECK(strcmp(q.dest, "/y z.bin") == 0);
}

void test_dav_knows_every_method_and_the_rest_is_unknown(void) {
  static const char *names[] = { "OPTIONS", "GET", "HEAD", "PUT", "DELETE",
    "MKCOL", "PROPFIND", "PROPPATCH", "MOVE", "COPY", "LOCK", "UNLOCK" };
  static const DavMethod ms[] = { DAV_OPTIONS, DAV_GET, DAV_HEAD, DAV_PUT,
    DAV_DELETE, DAV_MKCOL, DAV_PROPFIND, DAV_PROPPATCH, DAV_MOVE, DAV_COPY,
    DAV_LOCK, DAV_UNLOCK };
  char buf[64];
  DavRequest q;
  size_t i;
  for (i = 0; i < sizeof names / sizeof names[0]; i++) {
    sprintf(buf, "%s / HTTP/1.1\r\n\r\n", names[i]);
    CHECK_EQ(dav_parse(buf, strlen(buf), &q), 0);
    CHECK_EQ(q.method, ms[i]);
    CHECK(strcmp(dav_method_name(ms[i]), names[i]) == 0);
  }
  strcpy(buf, "BREW / HTTP/1.1\r\n\r\n");
  CHECK_EQ(dav_parse(buf, strlen(buf), &q), 0);
  CHECK_EQ(q.method, DAV_UNKNOWN);
}

void test_dav_rejects_a_malformed_request_line(void) {
  DavRequest q;
  const char *r = "GET\r\n\r\n";
  CHECK_EQ(dav_parse(r, strlen(r), &q), -1);
  CHECK_EQ(q.bad, 400);
  r = "GET /a HTTP/1.1";                     /* no terminator at all */
  CHECK_EQ(dav_parse(r, strlen(r), &q), -1);
  CHECK_EQ(q.bad, 400);
}

void test_dav_infinity_and_chunked_are_recorded_not_refused(void) {
  const char *r = "PROPFIND / HTTP/1.1\r\nDepth: infinity\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n";
  DavRequest q;
  CHECK_EQ(dav_parse(r, strlen(r), &q), 0);
  CHECK_EQ(q.depth, DAV_DEPTH_INFINITY);
  CHECK_EQ(q.chunked, 1);
}
```

- [ ] **Step 2: Run to verify they fail**

Add `${ROOT}/kernel/net/dav.c` to `host/CMakeLists.txt` on the line after `manifest.c`. Create an empty `kernel/net/dav.c` and a `dav.h` containing only the include guard.
Run: `build\hostbuild.bat`
Expected: build FAILS (implicit declaration of `dav_headers_end` is an error under `/we4013`).

- [ ] **Step 3: Write `dav.h` and the parser**

`kernel/net/dav.h`:

```c
/* WebDAV, the part that is not a socket.
 *
 * Portable C so the host suite can run it: the request parser, the path
 * decoder that is the one thing the server must not get wrong, the two date
 * formats, and the XML a PROPFIND answer is made of. kernel/net/share.c is
 * the device-side glue that owns the socket and the task and calls these.
 *
 * Only the headers the server acts on are read; everything else is skipped.
 * Header names are matched without case, as RFC 9110 requires. */
#ifndef CARDOS_DAV_H
#define CARDOS_DAV_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs/path.h"

typedef enum {
  DAV_UNKNOWN = 0, DAV_OPTIONS, DAV_GET, DAV_HEAD, DAV_PUT, DAV_DELETE,
  DAV_MKCOL, DAV_PROPFIND, DAV_PROPPATCH, DAV_MOVE, DAV_COPY, DAV_LOCK,
  DAV_UNLOCK
} DavMethod;

#define DAV_DEPTH_INFINITY (-1)

typedef struct {
  DavMethod method;
  char     path[FS_PATH_MAX];   /* decoded, normalised CardOS path */
  char     dest[FS_PATH_MAX];   /* Destination, same treatment, or "" */
  int      depth;               /* 0, 1, or DAV_DEPTH_INFINITY (the default) */
  uint32_t content_length;
  int      has_content_length;
  int      chunked;             /* Transfer-Encoding: chunked */
  int      expect_continue;     /* Expect: 100-continue */
  int      keep_alive;          /* 1 unless Connection: close */
  int      overwrite;           /* 1 unless Overwrite: F */
  int      bad;                 /* 0, or the status to answer with instead */
} DavRequest;

/* Index just past the blank line that ends the headers, or -1 if `buf` does
 * not contain one yet. */
int dav_headers_end(const char *buf, size_t n);

/* Parse the request line and headers in hdr[0..n). Returns 0, or -1 with
 * req->bad set to the status to send: 400 for a line that is not HTTP, 403
 * for a path that would leave the card, 414 for one that does not fit. */
int dav_parse(const char *hdr, size_t n, DavRequest *req);

const char *dav_method_name(DavMethod m);

#endif /* CARDOS_DAV_H */
```

`kernel/net/dav.c` (this task's portion; later tasks append):

```c
/* WebDAV without the socket. See dav.h. */

#include "kernel/net/dav.h"

#include <stdio.h>
#include <string.h>

static const struct { const char *name; DavMethod m; } METHODS[] = {
  { "OPTIONS", DAV_OPTIONS }, { "GET", DAV_GET }, { "HEAD", DAV_HEAD },
  { "PUT", DAV_PUT }, { "DELETE", DAV_DELETE }, { "MKCOL", DAV_MKCOL },
  { "PROPFIND", DAV_PROPFIND }, { "PROPPATCH", DAV_PROPPATCH },
  { "MOVE", DAV_MOVE }, { "COPY", DAV_COPY }, { "LOCK", DAV_LOCK },
  { "UNLOCK", DAV_UNLOCK },
};
#define NMETHODS ((int)(sizeof METHODS / sizeof METHODS[0]))

const char *dav_method_name(DavMethod m) {
  int i;
  for (i = 0; i < NMETHODS; i++) if (METHODS[i].m == m) return METHODS[i].name;
  return "?";
}

int dav_headers_end(const char *buf, size_t n) {
  size_t i;
  for (i = 3; i < n; i++)
    if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n')
      return (int)i + 1;
  return -1;
}

static int ieq(const char *a, const char *b, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
    if (x != y) return 0;
  }
  return 1;
}

/* Does the header line at `line` (length n) carry `name`? If so, return the
 * value with surrounding spaces trimmed, else NULL. */
static const char *header(const char *line, size_t n, const char *name,
                          size_t *vlen) {
  size_t nl = strlen(name);
  const char *v, *end = line + n;
  if (n < nl + 1 || line[nl] != ':' || !ieq(line, name, nl)) return NULL;
  v = line + nl + 1;
  while (v < end && (*v == ' ' || *v == '\t')) v++;
  while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
  *vlen = (size_t)(end - v);
  return v;
}

/* dav_decode_path is defined in the next task. */
int dav_decode_path(const char *in, size_t n, char *out, size_t out_size);

int dav_parse(const char *hdr, size_t n, DavRequest *req) {
  const char *p = hdr, *end = hdr + n, *sp1, *sp2, *eol;
  size_t vlen;
  int i, rc;

  memset(req, 0, sizeof *req);
  req->depth = DAV_DEPTH_INFINITY;
  req->keep_alive = 1;
  req->overwrite = 1;

  /* METHOD SP target SP HTTP/x.y CRLF */
  eol = memchr(p, '\n', (size_t)(end - p));
  if (!eol || eol == p || eol[-1] != '\r') { req->bad = 400; return -1; }
  sp1 = memchr(p, ' ', (size_t)(eol - p));
  if (!sp1) { req->bad = 400; return -1; }
  sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
  if (!sp2 || sp2 == sp1 + 1) { req->bad = 400; return -1; }

  req->method = DAV_UNKNOWN;
  for (i = 0; i < NMETHODS; i++) {
    size_t ml = strlen(METHODS[i].name);
    if ((size_t)(sp1 - p) == ml && memcmp(p, METHODS[i].name, ml) == 0) {
      req->method = METHODS[i].m;
      break;
    }
  }

  rc = dav_decode_path(sp1 + 1, (size_t)(sp2 - sp1 - 1), req->path, sizeof req->path);
  if (rc != 0) { req->bad = rc == -2 ? 414 : 403; return -1; }

  /* Headers, one per line, until the blank one. */
  p = eol + 1;
  while (p < end) {
    size_t len;
    const char *v;
    eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol) break;
    len = (size_t)(eol - p);
    if (len && p[len - 1] == '\r') len--;
    if (len == 0) break;

    if ((v = header(p, len, "Content-Length", &vlen)) != NULL) {
      uint32_t cl = 0;
      size_t k;
      for (k = 0; k < vlen && v[k] >= '0' && v[k] <= '9'; k++)
        cl = cl * 10 + (uint32_t)(v[k] - '0');
      req->content_length = cl;
      req->has_content_length = 1;
    } else if ((v = header(p, len, "Connection", &vlen)) != NULL) {
      if (vlen == 5 && ieq(v, "close", 5)) req->keep_alive = 0;
    } else if ((v = header(p, len, "Transfer-Encoding", &vlen)) != NULL) {
      if (vlen >= 7 && ieq(v, "chunked", 7)) req->chunked = 1;
    } else if ((v = header(p, len, "Expect", &vlen)) != NULL) {
      if (vlen == 12 && ieq(v, "100-continue", 12)) req->expect_continue = 1;
    } else if ((v = header(p, len, "Depth", &vlen)) != NULL) {
      if (vlen == 1 && v[0] == '0') req->depth = 0;
      else if (vlen == 1 && v[0] == '1') req->depth = 1;
      else req->depth = DAV_DEPTH_INFINITY;
    } else if ((v = header(p, len, "Overwrite", &vlen)) != NULL) {
      if (vlen == 1 && (v[0] == 'F' || v[0] == 'f')) req->overwrite = 0;
    } else if ((v = header(p, len, "Destination", &vlen)) != NULL) {
      rc = dav_decode_path(v, vlen, req->dest, sizeof req->dest);
      if (rc != 0) { req->bad = rc == -2 ? 414 : 403; return -1; }
    }
    p = eol + 1;
  }
  return 0;
}
```

For this task only, so the tests link, append a temporary stub at the end of `dav.c` — Task 3 replaces it:

```c
/* TEMPORARY: replaced in Task 3. */
int dav_decode_path(const char *in, size_t n, char *out, size_t out_size) {
  const char *s = in;
  size_t i, o = 0;
  if (n > 7 && memcmp(s, "http://", 7) == 0) {
    const char *slash = memchr(s + 7, '/', n - 7);
    if (!slash) return -1;
    n -= (size_t)(slash - s); s = slash;
  }
  for (i = 0; i < n && o + 1 < out_size; i++) {
    if (s[i] == '%' && i + 2 < n && s[i + 1] == '2' && s[i + 2] == '0') { out[o++] = ' '; i += 2; }
    else out[o++] = s[i];
  }
  out[o] = 0;
  return 0;
}
```

- [ ] **Step 4: Run to verify they pass**

Run: `build\hostbuild.bat`
Expected: `0 failures`, six new `test_dav_*` lines reporting `ok`.

- [ ] **Step 5: Commit**

```bash
rtk git add kernel/net/dav.h kernel/net/dav.c test/test_dav.c test/test_main.c host/CMakeLists.txt
rtk git commit -m "dav: parse a request line and the headers the server acts on

Portable, host-tested. The socket side comes later."
```

---

### Task 3: `dav.c` — paths, the one input the server must not trust

**Files:**
- Modify: `kernel/net/dav.h` (declare two functions), `kernel/net/dav.c` (replace the stub)
- Modify: `test/test_dav.c` (append)

**Interfaces:**
- Produces:

```c
/* Percent-decode in[0..n) -- a request target or a Destination, which may be
 * a full http://host/... URL -- into a normalised CardOS path. 0 on success;
 * -1 if it is not a path the card can hold (relative, traversal, a backslash,
 * a control character, a query string, a NUL); -2 if it does not fit. */
int dav_decode_path(const char *in, size_t n, char *out, size_t out_size);

/* The reverse, for an href: percent-encode everything but unreserved
 * characters and '/'. A directory gets a trailing '/' when is_dir. Returns
 * bytes written, or -1 if it did not fit. */
int dav_encode_path(const char *path, int is_dir, char *out, size_t out_size);
```

- [ ] **Step 1: Write the failing tests**

Append to `test/test_dav.c`:

```c
static int dec(const char *in, char *out, size_t n) {
  return dav_decode_path(in, strlen(in), out, n);
}

void test_dav_decodes_percent_escapes_and_normalises(void) {
  char out[FS_PATH_MAX];
  CHECK_EQ(dec("/desktop/a%20b.txt", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop/a b.txt") == 0);
  CHECK_EQ(dec("/desktop/", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop") == 0);
  CHECK_EQ(dec("//desktop///x", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop/x") == 0);
  CHECK_EQ(dec("/", out, sizeof out), 0);
  CHECK(strcmp(out, "/") == 0);
  CHECK_EQ(dec("/caf%C3%A9", out, sizeof out), 0);
  CHECK(strcmp(out, "/caf\xC3\xA9") == 0);
}

void test_dav_strips_the_host_from_a_full_url(void) {
  char out[FS_PATH_MAX];
  CHECK_EQ(dec("http://192.168.1.23/desktop/x", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop/x") == 0);
  CHECK_EQ(dec("http://cardos:80/", out, sizeof out), 0);
  CHECK(strcmp(out, "/") == 0);
}

void test_dav_refuses_every_shape_of_traversal(void) {
  char out[FS_PATH_MAX];
  CHECK_EQ(dec("/../x", out, sizeof out), -1);
  CHECK_EQ(dec("/desktop/../../x", out, sizeof out), -1);
  CHECK_EQ(dec("/%2e%2e/x", out, sizeof out), -1);
  CHECK_EQ(dec("/%2E%2E/x", out, sizeof out), -1);
  CHECK_EQ(dec("/desktop/..%2fx", out, sizeof out), -1);
  CHECK_EQ(dec("/desktop\\x", out, sizeof out), -1);
  CHECK_EQ(dec("/desktop%5cx", out, sizeof out), -1);
  CHECK_EQ(dec("/x%00y", out, sizeof out), -1);
  CHECK_EQ(dec("/x?y=1", out, sizeof out), -1);
  CHECK_EQ(dec("desktop/x", out, sizeof out), -1);
  CHECK_EQ(dec("", out, sizeof out), -1);
  CHECK_EQ(dec("/x%zz", out, sizeof out), -1);
  CHECK_EQ(dec("/x\x01", out, sizeof out), -1);
}

void test_dav_a_single_dot_segment_is_harmless(void) {
  char out[FS_PATH_MAX];
  CHECK_EQ(dec("/desktop/./x", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop/x") == 0);
}

void test_dav_a_path_that_does_not_fit_is_414_not_truncated(void) {
  char in[300], out[FS_PATH_MAX];
  memset(in, 'a', sizeof in);
  in[0] = '/';
  in[sizeof in - 1] = 0;
  CHECK_EQ(dav_decode_path(in, strlen(in), out, sizeof out), -2);
}

void test_dav_encodes_an_href(void) {
  char out[256];
  CHECK(dav_encode_path("/desktop/a b.txt", 0, out, sizeof out) > 0);
  CHECK(strcmp(out, "/desktop/a%20b.txt") == 0);
  CHECK(dav_encode_path("/desktop", 1, out, sizeof out) > 0);
  CHECK(strcmp(out, "/desktop/") == 0);
  CHECK(dav_encode_path("/", 1, out, sizeof out) > 0);
  CHECK(strcmp(out, "/") == 0);
  CHECK(dav_encode_path("/caf\xC3\xA9&<>", 0, out, sizeof out) > 0);
  CHECK(strcmp(out, "/caf%C3%A9%26%3C%3E") == 0);
  CHECK_EQ(dav_encode_path("/desktop/a b", 0, out, 12), -1);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `build\hostbuild.bat`
Expected: build fails on `dav_encode_path` (undeclared); after adding just the declaration to `dav.h`, the link fails. Either is the expected red.

- [ ] **Step 3: Replace the stub with the real decoder and add the encoder**

Add the two declarations from the Interfaces block to `dav.h` (below `dav_parse`). Remove the forward declaration of `dav_decode_path` from `dav.c` and the TEMPORARY stub, and add:

```c
static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int dav_decode_path(const char *in, size_t n, char *out, size_t out_size) {
  char raw[FS_PATH_MAX];
  size_t i, o = 0;

  /* A Destination may be absolute: skip scheme and host. */
  if (n > 7 && memcmp(in, "http://", 7) == 0) {
    const char *slash = memchr(in + 7, '/', n - 7);
    if (!slash) return -1;
    n -= (size_t)(slash - in);
    in = slash;
  }
  if (n == 0 || in[0] != '/') return -1;

  for (i = 0; i < n; i++) {
    char c = in[i];
    if (c == '%') {
      int hi, lo;
      if (i + 2 >= n) return -1;
      hi = hexval(in[i + 1]);
      lo = hexval(in[i + 2]);
      if (hi < 0 || lo < 0) return -1;
      c = (char)(hi * 16 + lo);
      i += 2;
    } else if (c == '?' || c == '#') {
      return -1;                       /* a file has no query string */
    }
    if (c == '\0' || c == '\\' || (unsigned char)c < 0x20 || c == 0x7f) return -1;
    if (o + 1 >= sizeof raw) return -2;
    raw[o++] = c;
  }
  raw[o] = '\0';

  /* Traversal: refuse rather than resolve. path_normalize would clamp ".."
   * at the root, and a client that sends ".." is not one to be helpful to. */
  {
    const char *s = raw;
    while (*s) {
      const char *seg = s, *e = strchr(s, '/');
      size_t len = e ? (size_t)(e - seg) : strlen(seg);
      if (len == 2 && seg[0] == '.' && seg[1] == '.') return -1;
      s = e ? e + 1 : seg + len;
    }
  }
  if (path_normalize(raw, out, out_size) != 0) return -2;
  return 0;
}

static int unreserved(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

int dav_encode_path(const char *path, int is_dir, char *out, size_t out_size) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t o = 0;
  const unsigned char *p = (const unsigned char *)path;

  for (; *p; p++) {
    if (*p == '/' || unreserved(*p)) {
      if (o + 1 >= out_size) return -1;
      out[o++] = (char)*p;
    } else {
      if (o + 3 >= out_size) return -1;
      out[o++] = '%';
      out[o++] = HEX[*p >> 4];
      out[o++] = HEX[*p & 15];
    }
  }
  if (is_dir && !(o == 1 && out[0] == '/')) {
    if (o + 1 >= out_size) return -1;
    out[o++] = '/';
  }
  out[o] = '\0';
  return (int)o;
}
```

- [ ] **Step 4: Run to verify they pass**

Run: `build\hostbuild.bat`
Expected: `0 failures`; all `test_dav_*` ok, including the Task 2 tests (the `Destination` test relied on the stub's `%20` handling and now goes through the real decoder).

- [ ] **Step 5: Commit**

```bash
rtk git add kernel/net/dav.h kernel/net/dav.c test/test_dav.c test/test_main.c
rtk git commit -m "dav: decode and encode paths, refusing every shape of traversal"
```

---

### Task 4: `dav.c` — dates

**Files:**
- Modify: `kernel/net/dav.h`, `kernel/net/dav.c`, `test/test_dav.c`

**Interfaces:**
- Produces:

```c
/* "Sun, 06 Nov 1994 08:49:37 GMT" -- for getlastmodified and Last-Modified. */
void dav_http_date(uint32_t epoch, char *out, size_t out_size);
/* "1994-11-06T08:49:37Z" -- for creationdate. */
void dav_iso_date(uint32_t epoch, char *out, size_t out_size);
```

Both take UTC seconds and format UTC; there is no zone and so no DST question. Epoch 0 formats as 1970, which is what a file with no timestamp shows.

- [ ] **Step 1: Write the failing tests**

Append to `test/test_dav.c`:

```c
void test_dav_formats_the_rfc_example_date(void) {
  char out[40];
  dav_http_date(784111777u, out, sizeof out);     /* the RFC 7231 example */
  CHECK(strcmp(out, "Sun, 06 Nov 1994 08:49:37 GMT") == 0);
  dav_iso_date(784111777u, out, sizeof out);
  CHECK(strcmp(out, "1994-11-06T08:49:37Z") == 0);
}

void test_dav_formats_the_epoch_and_a_leap_day(void) {
  char out[40];
  dav_http_date(0, out, sizeof out);
  CHECK(strcmp(out, "Thu, 01 Jan 1970 00:00:00 GMT") == 0);
  dav_http_date(1709164800u, out, sizeof out);    /* 2024-02-29 00:00:00 */
  CHECK(strcmp(out, "Thu, 29 Feb 2024 00:00:00 GMT") == 0);
  dav_http_date(1789603199u, out, sizeof out);    /* 2026-09-17 23:59:59 */
  CHECK(strcmp(out, "Thu, 17 Sep 2026 23:59:59 GMT") == 0);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `build\hostbuild.bat`
Expected: build fails, `dav_http_date` undeclared.

- [ ] **Step 3: Implement**

Add the declarations to `dav.h`, and to `dav.c`:

```c
/* Civil date from days since 1970-01-01 (Howard Hinnant's algorithm), so
 * there is no dependence on libc's gmtime -- the device's newlib is built
 * with a zone, and the host's is not. */
static void civil(uint32_t days, int *y, int *m, int *d) {
  int64_t z = (int64_t)days + 719468;
  int64_t era = z / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2));
}

static const char *const DAYS[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
static const char *const MONS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void dav_http_date(uint32_t epoch, char *out, size_t out_size) {
  uint32_t days = epoch / 86400u, secs = epoch % 86400u;
  int y, m, d;
  civil(days, &y, &m, &d);
  snprintf(out, out_size, "%s, %02d %s %04d %02u:%02u:%02u GMT",
           DAYS[days % 7], d, MONS[m - 1], y,
           (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}

void dav_iso_date(uint32_t epoch, char *out, size_t out_size) {
  uint32_t days = epoch / 86400u, secs = epoch % 86400u;
  int y, m, d;
  civil(days, &y, &m, &d);
  snprintf(out, out_size, "%04d-%02d-%02dT%02u:%02u:%02uZ", y, m, d,
           (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}
```

- [ ] **Step 4: Run to verify they pass**

Run: `build\hostbuild.bat` — expected `0 failures`.

- [ ] **Step 5: Commit**

```bash
rtk git add kernel/net/dav.h kernel/net/dav.c test/test_dav.c test/test_main.c
rtk git commit -m "dav: the two date formats, without gmtime"
```

---

### Task 5: `dav.c` — what goes back: response heads, PROPFIND XML, the lock

**Files:**
- Modify: `kernel/net/dav.h`, `kernel/net/dav.c`, `test/test_dav.c`

**Interfaces:**
- Produces:

```c
#define DAV_ALLOW "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, MOVE, COPY, LOCK, UNLOCK"

const char *dav_status_text(int status);            /* "Not Found" */
const char *dav_content_type(const char *path);     /* by extension; "application/octet-stream" */

/* A complete status line + headers + blank line. content_length < 0 means
 * chunked. `extra` is zero or more complete "Name: value\r\n" lines. Returns
 * bytes written, or -1 if it did not fit. */
int dav_response_head(int status, int32_t content_length, const char *content_type,
                      const char *extra, int keep_alive, char *out, size_t out_size);

typedef struct {
  const char *path;       /* full CardOS path of the entry */
  uint32_t    size;
  int         is_dir;
  uint32_t    mtime;
} DavEntry;

extern const char DAV_MULTISTATUS_HEAD[];
extern const char DAV_MULTISTATUS_TAIL[];

/* One <D:response> for an entry. Returns bytes written, or -1. */
int dav_propfind_entry(const DavEntry *e, char *out, size_t out_size);

/* The 207 body for PROPPATCH: says every property was set. */
int dav_proppatch_body(const char *path, char *out, size_t out_size);

/* The 200 body for LOCK, and the token it names for the Lock-Token header. */
#define DAV_LOCK_TOKEN "opaquelocktoken:cardos-0000-0000-0001"
int dav_lock_body(const char *path, char *out, size_t out_size);
```

- [ ] **Step 1: Write the failing tests**

Append to `test/test_dav.c`:

```c
void test_dav_response_head_has_the_headers_windows_needs(void) {
  char out[512];
  int n = dav_response_head(200, 0, NULL,
    "DAV: 1,2\r\nMS-Author-Via: DAV\r\nAllow: " DAV_ALLOW "\r\n", 1, out, sizeof out);
  CHECK(n > 0);
  CHECK(strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0);
  CHECK(strstr(out, "Content-Length: 0\r\n") != NULL);
  CHECK(strstr(out, "DAV: 1,2\r\n") != NULL);
  CHECK(strstr(out, "MS-Author-Via: DAV\r\n") != NULL);
  CHECK(strstr(out, "Connection: keep-alive\r\n") != NULL);
  CHECK(strstr(out, "Server: CardOS\r\n") != NULL);
  CHECK(strcmp(out + n - 4, "\r\n\r\n") == 0);

  n = dav_response_head(207, -1, "text/xml; charset=\"utf-8\"", NULL, 0, out, sizeof out);
  CHECK(n > 0);
  CHECK(strncmp(out, "HTTP/1.1 207 Multi-Status\r\n", 27) == 0);
  CHECK(strstr(out, "Transfer-Encoding: chunked\r\n") != NULL);
  CHECK(strstr(out, "Content-Length:") == NULL);
  CHECK(strstr(out, "Connection: close\r\n") != NULL);
  CHECK(strstr(out, "Content-Type: text/xml; charset=\"utf-8\"\r\n") != NULL);

  CHECK_EQ(dav_response_head(200, 0, NULL, NULL, 1, out, 20), -1);
}

void test_dav_knows_its_status_texts(void) {
  CHECK(strcmp(dav_status_text(100), "Continue") == 0);
  CHECK(strcmp(dav_status_text(201), "Created") == 0);
  CHECK(strcmp(dav_status_text(204), "No Content") == 0);
  CHECK(strcmp(dav_status_text(207), "Multi-Status") == 0);
  CHECK(strcmp(dav_status_text(403), "Forbidden") == 0);
  CHECK(strcmp(dav_status_text(404), "Not Found") == 0);
  CHECK(strcmp(dav_status_text(405), "Method Not Allowed") == 0);
  CHECK(strcmp(dav_status_text(409), "Conflict") == 0);
  CHECK(strcmp(dav_status_text(411), "Length Required") == 0);
  CHECK(strcmp(dav_status_text(412), "Precondition Failed") == 0);
  CHECK(strcmp(dav_status_text(414), "URI Too Long") == 0);
  CHECK(strcmp(dav_status_text(415), "Unsupported Media Type") == 0);
  CHECK(strcmp(dav_status_text(431), "Request Header Fields Too Large") == 0);
  CHECK(strcmp(dav_status_text(507), "Insufficient Storage") == 0);
  CHECK(strcmp(dav_status_text(599), "Unknown") == 0);
}

void test_dav_guesses_content_types(void) {
  CHECK(strcmp(dav_content_type("/a.txt"), "text/plain") == 0);
  CHECK(strcmp(dav_content_type("/a.md"), "text/plain") == 0);
  CHECK(strcmp(dav_content_type("/a.html"), "text/html") == 0);
  CHECK(strcmp(dav_content_type("/a.png"), "image/png") == 0);
  CHECK(strcmp(dav_content_type("/a.wav"), "audio/wav") == 0);
  CHECK(strcmp(dav_content_type("/a.capp"), "application/octet-stream") == 0);
  CHECK(strcmp(dav_content_type("/noext"), "application/octet-stream") == 0);
  CHECK(strcmp(dav_content_type("/a.TXT"), "text/plain") == 0);
}

void test_dav_propfind_entry_for_a_file(void) {
  DavEntry e = { "/desktop/a b.txt", 1234, 0, 784111777u };
  char out[1024];
  int n = dav_propfind_entry(&e, out, sizeof out);
  CHECK(n > 0);
  CHECK(strcmp(out,
    "<D:response>"
    "<D:href>/desktop/a%20b.txt</D:href>"
    "<D:propstat><D:prop>"
    "<D:resourcetype/>"
    "<D:getcontentlength>1234</D:getcontentlength>"
    "<D:getlastmodified>Sun, 06 Nov 1994 08:49:37 GMT</D:getlastmodified>"
    "<D:creationdate>1994-11-06T08:49:37Z</D:creationdate>"
    "<D:displayname>a b.txt</D:displayname>"
    "<D:getcontenttype>text/plain</D:getcontenttype>"
    "<D:getetag>\"4d2-2ebca9a1\"</D:getetag>"
    "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
    "</D:response>") == 0);
}

void test_dav_propfind_entry_for_a_folder_and_the_root(void) {
  DavEntry d = { "/desktop", 0, 1, 0 };
  DavEntry r = { "/", 0, 1, 0 };
  char out[1024];
  CHECK(dav_propfind_entry(&d, out, sizeof out) > 0);
  CHECK(strstr(out, "<D:href>/desktop/</D:href>") != NULL);
  CHECK(strstr(out, "<D:resourcetype><D:collection/></D:resourcetype>") != NULL);
  CHECK(strstr(out, "<D:displayname>desktop</D:displayname>") != NULL);
  CHECK(strstr(out, "getcontentlength") == NULL);
  CHECK(strstr(out, "getcontenttype") == NULL);
  CHECK(dav_propfind_entry(&r, out, sizeof out) > 0);
  CHECK(strstr(out, "<D:href>/</D:href>") != NULL);
  CHECK(strstr(out, "<D:displayname></D:displayname>") != NULL);
  CHECK_EQ(dav_propfind_entry(&d, out, 64), -1);
}

void test_dav_escapes_xml_in_names(void) {
  DavEntry e = { "/a&b<c>.txt", 1, 0, 0 };
  char out[1024];
  CHECK(dav_propfind_entry(&e, out, sizeof out) > 0);
  CHECK(strstr(out, "<D:displayname>a&amp;b&lt;c&gt;.txt</D:displayname>") != NULL);
  CHECK(strstr(out, "<D:href>/a%26b%3Cc%3E.txt</D:href>") != NULL);
}

void test_dav_multistatus_wrapper_and_the_yes_bodies(void) {
  char out[1024];
  CHECK(strstr(DAV_MULTISTATUS_HEAD, "<D:multistatus xmlns:D=\"DAV:\">") != NULL);
  CHECK(strcmp(DAV_MULTISTATUS_TAIL, "</D:multistatus>") == 0);

  CHECK(dav_proppatch_body("/x y", out, sizeof out) > 0);
  CHECK(strstr(out, "<D:href>/x%20y</D:href>") != NULL);
  CHECK(strstr(out, "<D:status>HTTP/1.1 200 OK</D:status>") != NULL);

  CHECK(dav_lock_body("/x", out, sizeof out) > 0);
  CHECK(strstr(out, "<D:locktoken><D:href>" DAV_LOCK_TOKEN "</D:href></D:locktoken>") != NULL);
  CHECK(strstr(out, "<D:lockscope><D:exclusive/></D:lockscope>") != NULL);
  CHECK(strstr(out, "<D:href>/x</D:href>") != NULL);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `build\hostbuild.bat` — expected: build fails on undeclared functions.

- [ ] **Step 3: Implement**

Add the Interfaces block to `dav.h`. In `dav.c`:

```c
const char *dav_status_text(int status) {
  switch (status) {
  case 100: return "Continue";
  case 200: return "OK";
  case 201: return "Created";
  case 204: return "No Content";
  case 207: return "Multi-Status";
  case 400: return "Bad Request";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 411: return "Length Required";
  case 412: return "Precondition Failed";
  case 414: return "URI Too Long";
  case 415: return "Unsupported Media Type";
  case 431: return "Request Header Fields Too Large";
  case 500: return "Internal Server Error";
  case 507: return "Insufficient Storage";
  default:  return "Unknown";
  }
}

const char *dav_content_type(const char *path) {
  static const struct { const char *ext, *type; } T[] = {
    { "txt", "text/plain" }, { "md", "text/plain" }, { "c", "text/plain" },
    { "h", "text/plain" }, { "py", "text/plain" }, { "csv", "text/plain" },
    { "html", "text/html" }, { "htm", "text/html" }, { "json", "application/json" },
    { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
    { "gif", "image/gif" }, { "bmp", "image/bmp" }, { "wav", "audio/wav" },
    { "pdf", "application/pdf" },
  };
  const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
  size_t i;
  if (!dot || (slash && slash > dot)) return "application/octet-stream";
  dot++;
  for (i = 0; i < sizeof T / sizeof T[0]; i++)
    if (strlen(T[i].ext) == strlen(dot) && ieq(dot, T[i].ext, strlen(dot)))
      return T[i].type;
  return "application/octet-stream";
}

int dav_response_head(int status, int32_t content_length, const char *content_type,
                      const char *extra, int keep_alive, char *out, size_t out_size) {
  int n = snprintf(out, out_size,
    "HTTP/1.1 %d %s\r\nServer: CardOS\r\nConnection: %s\r\n",
    status, dav_status_text(status), keep_alive ? "keep-alive" : "close");
  if (n < 0 || (size_t)n >= out_size) return -1;
  if (content_length >= 0)
    n += snprintf(out + n, out_size - (size_t)n, "Content-Length: %ld\r\n", (long)content_length);
  else
    n += snprintf(out + n, out_size - (size_t)n, "Transfer-Encoding: chunked\r\n");
  if ((size_t)n >= out_size) return -1;
  if (content_type)
    n += snprintf(out + n, out_size - (size_t)n, "Content-Type: %s\r\n", content_type);
  if ((size_t)n >= out_size) return -1;
  if (extra)
    n += snprintf(out + n, out_size - (size_t)n, "%s", extra);
  if ((size_t)n >= out_size) return -1;
  n += snprintf(out + n, out_size - (size_t)n, "\r\n");
  if ((size_t)n >= out_size) return -1;
  return n;
}

/* ---- XML ------------------------------------------------------------ */

const char DAV_MULTISTATUS_HEAD[] =
  "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:multistatus xmlns:D=\"DAV:\">";
const char DAV_MULTISTATUS_TAIL[] = "</D:multistatus>";

/* Append to a bounded buffer. Every writer below goes through this so a
 * response that would not fit fails as a whole rather than truncating. */
typedef struct { char *buf; size_t cap, len; int overflow; } Out;

static void put(Out *o, const char *s) {
  size_t n = strlen(s);
  if (o->overflow || o->len + n + 1 > o->cap) { o->overflow = 1; return; }
  memcpy(o->buf + o->len, s, n + 1);
  o->len += n;
}

static void put_xml(Out *o, const char *s) {
  char one[2] = { 0, 0 };
  for (; *s; s++) {
    if (*s == '&') put(o, "&amp;");
    else if (*s == '<') put(o, "&lt;");
    else if (*s == '>') put(o, "&gt;");
    else { one[0] = *s; put(o, one); }
  }
}

static void put_href(Out *o, const char *path, int is_dir) {
  char href[FS_PATH_MAX * 3];
  if (dav_encode_path(path, is_dir, href, sizeof href) < 0) { o->overflow = 1; return; }
  put(o, "<D:href>");
  put_xml(o, href);
  put(o, "</D:href>");
}

static int finish(Out *o) { return o->overflow ? -1 : (int)o->len; }

int dav_propfind_entry(const DavEntry *e, char *out, size_t out_size) {
  Out o = { out, out_size, 0, 0 };
  char tmp[64];

  put(&o, "<D:response>");
  put_href(&o, e->path, e->is_dir);
  put(&o, "<D:propstat><D:prop>");
  if (e->is_dir) {
    put(&o, "<D:resourcetype><D:collection/></D:resourcetype>");
  } else {
    put(&o, "<D:resourcetype/>");
    snprintf(tmp, sizeof tmp, "<D:getcontentlength>%lu</D:getcontentlength>",
             (unsigned long)e->size);
    put(&o, tmp);
  }
  put(&o, "<D:getlastmodified>");
  dav_http_date(e->mtime, tmp, sizeof tmp);
  put(&o, tmp);
  put(&o, "</D:getlastmodified><D:creationdate>");
  dav_iso_date(e->mtime, tmp, sizeof tmp);
  put(&o, tmp);
  put(&o, "</D:creationdate><D:displayname>");
  put_xml(&o, path_basename(e->path));
  put(&o, "</D:displayname>");
  if (!e->is_dir) {
    put(&o, "<D:getcontenttype>");
    put(&o, dav_content_type(e->path));
    put(&o, "</D:getcontenttype>");
  }
  snprintf(tmp, sizeof tmp, "<D:getetag>\"%lx-%lx\"</D:getetag>",
           (unsigned long)e->size, (unsigned long)e->mtime);
  put(&o, tmp);
  put(&o, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>");
  put(&o, "</D:response>");
  return finish(&o);
}

int dav_proppatch_body(const char *path, char *out, size_t out_size) {
  Out o = { out, out_size, 0, 0 };
  put(&o, DAV_MULTISTATUS_HEAD);
  put(&o, "<D:response>");
  put_href(&o, path, 0);
  put(&o, "<D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
          "</D:response>");
  put(&o, DAV_MULTISTATUS_TAIL);
  return finish(&o);
}

int dav_lock_body(const char *path, char *out, size_t out_size) {
  Out o = { out, out_size, 0, 0 };
  put(&o, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
          "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
          "<D:locktype><D:write/></D:locktype>"
          "<D:lockscope><D:exclusive/></D:lockscope>"
          "<D:depth>infinity</D:depth>"
          "<D:timeout>Second-3600</D:timeout>"
          "<D:locktoken><D:href>" DAV_LOCK_TOKEN "</D:href></D:locktoken>"
          "<D:lockroot>");
  put_href(&o, path, 0);
  put(&o, "</D:lockroot></D:activelock></D:lockdiscovery></D:prop>");
  return finish(&o);
}
```

Note `ieq` is the static helper from Task 2 — it must appear above `dav_content_type` in the file (it does, if you append in order).

- [ ] **Step 4: Run to verify they pass**

Run: `build\hostbuild.bat` — expected `0 failures`. If `test_dav_propfind_entry_for_a_file` disagrees on the etag, the format is `"%lx-%lx"` of size then mtime: 1234 = `4d2`, 784111777 = `2ebca9a1`.

- [ ] **Step 5: Commit**

```bash
rtk git add kernel/net/dav.h kernel/net/dav.c test/test_dav.c test/test_main.c
rtk git commit -m "dav: response heads, the PROPFIND entry, and the bodies that say yes"
```

---

### Task 6: `share.c` — the task, the socket, and every method

**Files:**
- Create: `kernel/net/share.h`, `kernel/net/share.c`
- Modify: `src/CMakeLists.txt:33` — add `"../kernel/net/share.c"` after `httpq.c`
- Modify: `kernel/app/capprun.c:289-294` — call `share_app_closed()` in `release_slot`; add `#include "kernel/net/share.h"`

**Interfaces:**
- Consumes: everything in `dav.h`; `fs.h` (with `mtime` and the new `FsDir`); `wifi_ip()`, `wifi_is_connected()` from `wifi.h`; `heap_caps_get_free_size(MALLOC_CAP_8BIT)`.
- Produces (`share.h`):

```c
/* Start serving. `owned_by_app` records that an app started it, so that the
 * app going away stops it (share_app_closed). 0 on success; -1 and
 * share_error() says why: no wifi, no card, too little memory, already on. */
int  share_start(int owned_by_app);
void share_stop(void);
int  share_running(void);
const char *share_url(void);        /* "http://192.168.1.23/" or "" */
const char *share_error(void);      /* the last reason share_start refused */
/* Next log line -- "PUT /desktop/x.capp 201" -- or NULL. One per call; the
 * string is valid until the next call. */
const char *share_take_log(void);
/* capprun: an app was released. Stops the share if an app owned it. */
void share_app_closed(void);
```

No host test — this file is sockets and FreeRTOS. Verification is the firmware build here, and Task 9's client test against the device.

- [ ] **Step 1: Write `share.h`**

```c
/* The card as a network drive. Device-only.
 *
 * A WebDAV server on port 80, one connection at a time, on a task of its own
 * so a file being written does not freeze the shell. Everything that is not
 * a socket is in kernel/net/dav.c and is tested on the host; this file is
 * the glue and is deliberately thin.
 *
 * THE RULE, from bg.h: this task never touches the display. It says what it
 * did through share_take_log, and the shell draws that.
 *
 * On while you want it and off otherwise. There is no authentication -- the
 * boundary is the LAN, the same one webproxy.py trusts -- which is why this
 * is a thing you turn on rather than a service. */
#ifndef CARDOS_SHARE_H
#define CARDOS_SHARE_H

int  share_start(int owned_by_app);
void share_stop(void);
int  share_running(void);
const char *share_url(void);
const char *share_error(void);
const char *share_take_log(void);
void share_app_closed(void);

#endif /* CARDOS_SHARE_H */
```

- [ ] **Step 2: Write `share.c`**

```c
/* WebDAV on a socket. See share.h; the protocol is in dav.c. */

#include "kernel/net/share.h"
#include "kernel/net/dav.h"
#include "kernel/net/wifi.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "share";

#define PORT         80
#define BACKLOG      4
#define HDR_MAX      2048       /* request line and headers */
#define IO_MAX       4096       /* one transfer buffer, both directions */
#define HEAD_MAX     512        /* a response head */
#define XML_MAX      1024       /* one PROPFIND entry */
#define IDLE_MS      5000       /* a kept-alive connection that says nothing */
#define MIN_FREE     (40 * 1024)
#define LOG_LINES    8
#define LOG_MAX      64
#define MAX_DEPTH    8          /* recursive delete: levels of folder */

#define S_PRIORITY   3
#define S_STACK      8192
#define S_CORE       1

typedef struct {
  char hdr[HDR_MAX];
  char io[IO_MAX];
  char head[HEAD_MAX];
  char xml[XML_MAX];
} Bufs;

static TaskHandle_t  s_task;
static QueueHandle_t s_log;
static Bufs         *s_b;
static volatile int  s_stop, s_done, s_running;
static int           s_owned_by_app;
static char          s_url[48];
static char          s_error[96];
static char          s_logline[LOG_MAX];

/* ---- state the shell reads ------------------------------------------- */

int  share_running(void) { return s_running; }
const char *share_url(void) { return s_running ? s_url : ""; }
const char *share_error(void) { return s_error; }

const char *share_take_log(void) {
  if (!s_log || xQueueReceive(s_log, s_logline, 0) != pdTRUE) return NULL;
  return s_logline;
}

/* One line for the shell to show. Never blocks: a full queue means nobody is
 * looking, and dropping news is better than stalling a transfer. */
static void note(const char *method, const char *path) {
  char line[LOG_MAX];
  snprintf(line, sizeof line, "%s %s", method, path);
  xQueueSend(s_log, line, 0);
}

/* ---- the wire --------------------------------------------------------- */

static int send_all(int fd, const void *p, size_t n) {
  const char *c = p;
  while (n) {
    int w = send(fd, c, n, 0);
    if (w <= 0) return -1;
    c += w; n -= (size_t)w;
  }
  return 0;
}

static int send_head(int fd, int status, int32_t len, const char *type,
                     const char *extra, int keep) {
  int n = dav_response_head(status, len, type, extra, keep, s_b->head, HEAD_MAX);
  if (n < 0) return -1;
  return send_all(fd, s_b->head, (size_t)n);
}

static int send_chunk(int fd, const char *p, size_t n) {
  char sz[16];
  int k = snprintf(sz, sizeof sz, "%x\r\n", (unsigned)n);
  if (send_all(fd, sz, (size_t)k)) return -1;
  if (n && send_all(fd, p, n)) return -1;
  return send_all(fd, "\r\n", 2);
}

static int send_last_chunk(int fd) { return send_all(fd, "0\r\n\r\n", 5); }

/* A status with no body. */
static int reply(int fd, int status, const char *extra, int keep) {
  return send_head(fd, status, 0, NULL, extra, keep);
}

/* Read and discard a request body we do not want. */
static int drain(int fd, uint32_t n) {
  while (n) {
    int r = recv(fd, s_b->io, n < IO_MAX ? n : IO_MAX, 0);
    if (r <= 0) return -1;
    n -= (uint32_t)r;
  }
  return 0;
}

/* ---- files ------------------------------------------------------------ */

static int join(const char *dir, const char *name, char *out, size_t n) {
  int k = strcmp(dir, "/") == 0 ? snprintf(out, n, "/%s", name)
                                : snprintf(out, n, "%s/%s", dir, name);
  return (k > 0 && (size_t)k < n) ? 0 : -1;
}

static int parent_exists(const char *path) {
  char dir[FS_PATH_MAX];
  FsStat st;
  if (path_dirname(path, dir, sizeof dir) != 0) return 0;
  return fs_stat(dir, &st) == 0 && st.is_dir;
}

static int remove_tree(const char *path, int level) {
  FsDir d;
  FsEntry e;
  char child[FS_PATH_MAX];
  FsStat st;

  if (fs_stat(path, &st) != 0) return -1;
  if (!st.is_dir) return fs_remove(path);
  if (level >= MAX_DEPTH) return -1;

  /* FatFs cannot remove entries from a directory being iterated, so this
   * takes one child per pass and reopens. Slow, but a delete is rare. */
  for (;;) {
    int found = 0;
    if (fs_opendir(path, &d) != 0) return -1;
    while (fs_readdir(&d, &e) == 1) {
      if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
      found = 1;
      break;
    }
    fs_closedir(&d);
    if (!found) break;
    if (join(path, e.name, child, sizeof child) != 0) return -1;
    if (remove_tree(child, level + 1) != 0) return -1;
  }
  return fs_remove(path);
}

static int copy_file(const char *from, const char *to) {
  int in = fs_open(from, FS_O_READ), out, n, rc = 0;
  if (in < 0) return -1;
  out = fs_open(to, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (out < 0) { fs_close(in); return -1; }
  while ((n = fs_read(in, s_b->io, IO_MAX)) > 0)
    if (fs_write(out, s_b->io, (size_t)n) != n) { rc = -1; break; }
  if (n < 0) rc = -1;
  fs_close(in);
  fs_close(out);
  if (rc) fs_remove(to);
  return rc;
}

/* ---- methods ---------------------------------------------------------- */

static int do_options(int fd, const DavRequest *q) {
  return reply(fd, 200,
    "DAV: 1,2\r\nMS-Author-Via: DAV\r\nAllow: " DAV_ALLOW "\r\n", q->keep_alive);
}

static int propfind_one(int fd, const char *path, const FsStat *st) {
  DavEntry e = { path, st->size, st->is_dir, st->mtime };
  int n = dav_propfind_entry(&e, s_b->xml, XML_MAX);
  if (n < 0) return -1;
  return send_chunk(fd, s_b->xml, (size_t)n);
}

static int do_propfind(int fd, const DavRequest *q) {
  FsStat st;
  FsDir d;
  FsEntry e;
  char child[FS_PATH_MAX];

  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (q->depth == DAV_DEPTH_INFINITY) return reply(fd, 403, NULL, q->keep_alive);
  if (fs_stat(q->path, &st) != 0) return reply(fd, 404, NULL, q->keep_alive);

  if (send_head(fd, 207, -1, "text/xml; charset=\"utf-8\"", NULL, q->keep_alive)) return -1;
  if (send_chunk(fd, DAV_MULTISTATUS_HEAD, strlen(DAV_MULTISTATUS_HEAD))) return -1;
  if (propfind_one(fd, q->path, &st)) return -1;

  if (q->depth == 1 && st.is_dir && fs_opendir(q->path, &d) == 0) {
    while (fs_readdir(&d, &e) == 1) {
      FsStat cs;
      if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
      if (join(q->path, e.name, child, sizeof child) != 0) continue;
      cs.size = e.size; cs.is_dir = e.is_dir; cs.mtime = e.mtime;
      if (propfind_one(fd, child, &cs)) { fs_closedir(&d); return -1; }
    }
    fs_closedir(&d);
  }
  if (send_chunk(fd, DAV_MULTISTATUS_TAIL, strlen(DAV_MULTISTATUS_TAIL))) return -1;
  return send_last_chunk(fd);
}

static int do_get(int fd, const DavRequest *q, int head_only) {
  FsStat st;
  char extra[96];
  int in, n;

  if (fs_stat(q->path, &st) != 0) return reply(fd, 404, NULL, q->keep_alive);
  if (st.is_dir) return reply(fd, 403, NULL, q->keep_alive);

  {
    char date[40];
    dav_http_date(st.mtime, date, sizeof date);
    snprintf(extra, sizeof extra, "Last-Modified: %s\r\nAccept-Ranges: none\r\n", date);
  }
  if (send_head(fd, 200, (int32_t)st.size, dav_content_type(q->path), extra, q->keep_alive))
    return -1;
  if (head_only) return 0;

  in = fs_open(q->path, FS_O_READ);
  if (in < 0) return -1;                       /* head already sent: drop the line */
  while ((n = fs_read(in, s_b->io, IO_MAX)) > 0)
    if (send_all(fd, s_b->io, (size_t)n)) { fs_close(in); return -1; }
  fs_close(in);
  return n < 0 ? -1 : 0;
}

static int do_put(int fd, const DavRequest *q) {
  FsStat st;
  int existed, out;
  uint32_t left;

  if (q->chunked || !q->has_content_length) return reply(fd, 411, NULL, 0);
  if (fs_stat(q->path, &st) == 0 && st.is_dir) return reply(fd, 405, NULL, 0);
  existed = fs_stat(q->path, &st) == 0;
  if (!parent_exists(q->path)) return reply(fd, 409, NULL, 0);

  out = fs_open(q->path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (out < 0) return reply(fd, 507, NULL, 0);

  if (q->expect_continue && send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25)) {
    fs_close(out); return -1;
  }
  left = q->content_length;
  while (left) {
    int r = recv(fd, s_b->io, left < IO_MAX ? left : IO_MAX, 0);
    if (r <= 0) { fs_close(out); fs_remove(q->path); return -1; }
    if (fs_write(out, s_b->io, (size_t)r) != r) {
      fs_close(out); fs_remove(q->path);
      return reply(fd, 507, NULL, 0);
    }
    left -= (uint32_t)r;
  }
  fs_close(out);
  return reply(fd, existed ? 204 : 201, NULL, q->keep_alive);
}

static int do_delete(int fd, const DavRequest *q) {
  FsStat st;
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (fs_stat(q->path, &st) != 0) return reply(fd, 404, NULL, q->keep_alive);
  if (strcmp(q->path, "/") == 0) return reply(fd, 403, NULL, q->keep_alive);
  if (remove_tree(q->path, 0) != 0) return reply(fd, 403, NULL, q->keep_alive);
  return reply(fd, 204, NULL, q->keep_alive);
}

static int do_mkcol(int fd, const DavRequest *q) {
  FsStat st;
  if (q->has_content_length && q->content_length) {
    drain(fd, q->content_length);
    return reply(fd, 415, NULL, q->keep_alive);
  }
  if (fs_stat(q->path, &st) == 0) return reply(fd, 405, NULL, q->keep_alive);
  if (!parent_exists(q->path)) return reply(fd, 409, NULL, q->keep_alive);
  if (fs_mkdir(q->path) != 0) return reply(fd, 507, NULL, q->keep_alive);
  return reply(fd, 201, NULL, q->keep_alive);
}

/* MOVE and COPY share their preamble: a Destination, a source that exists,
 * a parent for the destination, and the Overwrite rule. Returns 0 to go on,
 * or the status already sent. */
static int move_copy_check(int fd, const DavRequest *q, FsStat *src, int *dest_existed) {
  FsStat dst;
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (!q->dest[0]) { reply(fd, 400, NULL, q->keep_alive); return 400; }
  if (fs_stat(q->path, src) != 0) { reply(fd, 404, NULL, q->keep_alive); return 404; }
  if (strcmp(q->path, q->dest) == 0) { reply(fd, 403, NULL, q->keep_alive); return 403; }
  if (!parent_exists(q->dest)) { reply(fd, 409, NULL, q->keep_alive); return 409; }
  *dest_existed = fs_stat(q->dest, &dst) == 0;
  if (*dest_existed) {
    if (!q->overwrite) { reply(fd, 412, NULL, q->keep_alive); return 412; }
    if (remove_tree(q->dest, 0) != 0) { reply(fd, 403, NULL, q->keep_alive); return 403; }
  }
  return 0;
}

static int do_move(int fd, const DavRequest *q) {
  FsStat src;
  int existed, rc = move_copy_check(fd, q, &src, &existed);
  if (rc) return rc < 0 ? -1 : 0;
  if (fs_rename(q->path, q->dest) != 0) return reply(fd, 403, NULL, q->keep_alive);
  return reply(fd, existed ? 204 : 201, NULL, q->keep_alive);
}

static int do_copy(int fd, const DavRequest *q) {
  FsStat src;
  int existed, rc = move_copy_check(fd, q, &src, &existed);
  if (rc) return rc < 0 ? -1 : 0;
  if (src.is_dir) return reply(fd, 403, NULL, q->keep_alive);   /* the documented limit */
  if (copy_file(q->path, q->dest) != 0) return reply(fd, 507, NULL, q->keep_alive);
  return reply(fd, existed ? 204 : 201, NULL, q->keep_alive);
}

static int do_proppatch(int fd, const DavRequest *q) {
  FsStat st;
  int n;
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (fs_stat(q->path, &st) != 0) return reply(fd, 404, NULL, q->keep_alive);
  n = dav_proppatch_body(q->path, s_b->xml, XML_MAX);
  if (n < 0) return reply(fd, 500, NULL, 0);
  if (send_head(fd, 207, n, "text/xml; charset=\"utf-8\"", NULL, q->keep_alive)) return -1;
  return send_all(fd, s_b->xml, (size_t)n);
}

static int do_lock(int fd, const DavRequest *q) {
  int n;
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  n = dav_lock_body(q->path, s_b->xml, XML_MAX);
  if (n < 0) return reply(fd, 500, NULL, 0);
  if (send_head(fd, 200, n, "text/xml; charset=\"utf-8\"",
                "Lock-Token: <" DAV_LOCK_TOKEN ">\r\n", q->keep_alive)) return -1;
  return send_all(fd, s_b->xml, (size_t)n);
}

static int do_unlock(int fd, const DavRequest *q) {
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  return reply(fd, 204, NULL, q->keep_alive);
}

/* One request on an open connection. Returns 1 to keep the connection, 0 to
 * close it. */
static int serve_one(int fd) {
  DavRequest q;
  int have = 0, end = -1, rc;

  /* Headers first. A slow client sends them in pieces; a hostile one never
   * sends the blank line, and the size cap is what ends that. */
  while (end < 0) {
    int r;
    if (have >= HDR_MAX - 1) { reply(fd, 431, NULL, 0); return 0; }
    r = recv(fd, s_b->hdr + have, HDR_MAX - 1 - have, 0);
    if (r <= 0) return 0;                   /* idle timeout, or gone */
    have += r;
    end = dav_headers_end(s_b->hdr, (size_t)have);
  }
  /* Bytes after the headers are the start of the body; push them back by
   * remembering them. Windows never pipelines, and a body that arrives with
   * the headers is a small one, so a memmove into io is enough. */
  if (dav_parse(s_b->hdr, (size_t)end, &q) != 0) {
    note("refused", q.bad == 403 ? "(path)" : "(bad request)");
    reply(fd, q.bad, NULL, 0);
    return 0;
  }
  if (have > end) {
    /* Only OPTIONS/PROPFIND/LOCK bodies come this way in practice, and all
     * are drained rather than read; so reduce the drain by what we hold. */
    uint32_t extra = (uint32_t)(have - end);
    if (q.has_content_length) q.content_length -= extra < q.content_length ? extra : q.content_length;
  }

  switch (q.method) {
  case DAV_OPTIONS:   rc = do_options(fd, &q); break;
  case DAV_PROPFIND:  rc = do_propfind(fd, &q); break;
  case DAV_GET:       rc = do_get(fd, &q, 0); break;
  case DAV_HEAD:      rc = do_get(fd, &q, 1); break;
  case DAV_PUT:       rc = do_put(fd, &q); break;
  case DAV_DELETE:    rc = do_delete(fd, &q); break;
  case DAV_MKCOL:     rc = do_mkcol(fd, &q); break;
  case DAV_MOVE:      rc = do_move(fd, &q); break;
  case DAV_COPY:      rc = do_copy(fd, &q); break;
  case DAV_PROPPATCH: rc = do_proppatch(fd, &q); break;
  case DAV_LOCK:      rc = do_lock(fd, &q); break;
  case DAV_UNLOCK:    rc = do_unlock(fd, &q); break;
  default:
    if (q.has_content_length) drain(fd, q.content_length);
    rc = reply(fd, 405, "Allow: " DAV_ALLOW "\r\n", q.keep_alive);
    break;
  }
  note(dav_method_name(q.method), q.path);
  return rc == 0 && q.keep_alive;
}
```

Then the task and the start/stop:

```c
/* ---- the task --------------------------------------------------------- */

static void share_task(void *arg) {
  int lfd = -1, cfd = -1;
  struct sockaddr_in addr;
  int one = 1;
  struct timeval tv = { IDLE_MS / 1000, (IDLE_MS % 1000) * 1000 };
  (void)arg;

  lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) goto out;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) goto out;
  if (listen(lfd, BACKLOG) < 0) goto out;

  while (!s_stop) {
    fd_set rf;
    struct timeval poll = { 0, 200 * 1000 };
    FD_ZERO(&rf);
    FD_SET(lfd, &rf);
    if (select(lfd + 1, &rf, NULL, NULL, &poll) <= 0) continue;

    cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) continue;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    while (!s_stop && serve_one(cfd)) { }
    close(cfd);
    cfd = -1;
  }

out:
  if (cfd >= 0) close(cfd);
  if (lfd >= 0) close(lfd);
  s_done = 1;
  vTaskDelete(NULL);
}

int share_start(int owned_by_app) {
  if (s_running) { snprintf(s_error, sizeof s_error, "already sharing"); return -1; }
  if (!fs_mounted()) { snprintf(s_error, sizeof s_error, "no card mounted"); return -1; }
  if (!wifi_is_connected()) { snprintf(s_error, sizeof s_error, "not on wifi"); return -1; }
  {
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeb < MIN_FREE) {
      snprintf(s_error, sizeof s_error, "not enough memory: %u KB free, sharing needs %u",
               (unsigned)(freeb / 1024), (unsigned)(MIN_FREE / 1024));
      return -1;
    }
  }
  if (!s_log) s_log = xQueueCreate(LOG_LINES, LOG_MAX);
  s_b = malloc(sizeof *s_b);
  if (!s_log || !s_b) { snprintf(s_error, sizeof s_error, "no memory for buffers"); return -1; }

  snprintf(s_url, sizeof s_url, "http://%s/", wifi_ip());
  s_stop = 0;
  s_done = 0;
  s_owned_by_app = owned_by_app;
  if (xTaskCreatePinnedToCore(share_task, "share", S_STACK, NULL, S_PRIORITY,
                              &s_task, S_CORE) != pdPASS) {
    free(s_b); s_b = NULL;
    snprintf(s_error, sizeof s_error, "no memory for the task");
    return -1;
  }
  s_running = 1;
  s_error[0] = 0;
  ESP_LOGI(TAG, "sharing at %s", s_url);
  return 0;
}

void share_stop(void) {
  int waited = 0;
  if (!s_running) return;
  s_stop = 1;
  /* The task checks the flag every 200 ms between connections and between
   * requests; a transfer in flight finishes first. Give it a few seconds. */
  while (!s_done && waited < 6000) { vTaskDelay(pdMS_TO_TICKS(20)); waited += 20; }
  if (!s_done) { vTaskDelete(s_task); ESP_LOGW(TAG, "share task did not stop; deleted"); }
  s_task = NULL;
  free(s_b);
  s_b = NULL;
  s_running = 0;
  ESP_LOGI(TAG, "stopped");
}

void share_app_closed(void) {
  if (s_running && s_owned_by_app) share_stop();
}
```

- [ ] **Step 3: Wire it in**

`src/CMakeLists.txt`: add `"../kernel/net/share.c"` on the line after `"../kernel/net/httpq.c"`.

`kernel/app/capprun.c`: add `#include "kernel/net/share.h"` with the other kernel includes, and in `release_slot` after `s->has_ui = 0;` add `share_app_closed();`.

- [ ] **Step 4: Build the firmware**

Run: `python tools/build_apps.py` then `python -m platformio run`
Expected: success. Watch for: `lwip/sockets.h` needing the `lwip` component — it is pulled in by `esp_netif`/`esp_wifi` already; if the include is not found, add `lwip` to `REQUIRES` in `src/CMakeLists.txt`.

- [ ] **Step 5: Commit**

```bash
rtk git add kernel/net/share.h kernel/net/share.c src/CMakeLists.txt kernel/app/capprun.c
rtk git commit -m "share: a WebDAV server on its own task, one connection at a time

Sockets and FreeRTOS only; the protocol is dav.c. Stops itself when the app
that started it is released."
```

---

### Task 7: The `share` console command

**Files:**
- Modify: `src/shellcmd.h:31` (declare), `src/shellcmd.c` (append), `src/main.c:214` (dispatch), `:140-154` (help), `:297-303` (COMMANDS)

**Interfaces:**
- Consumes: `share_start(0)`, `share_stop()`, `share_running()`, `share_url()`, `share_error()`, `share_take_log()`.
- Produces: `void cmd_share(const char *arg);` — `share` starts (or reports if running), `share off` stops, `share log` prints queued log lines.

- [ ] **Step 1: Declare and dispatch**

`src/shellcmd.h`, after `cmd_update`:

```c
/* share        -- put the card on the LAN as a WebDAV drive, print the URL
 * share off    -- stop
 * share log    -- what has been asked for since last time */
void cmd_share(const char *arg);
```

`src/main.c`, after the `update` line: `else if (!strcmp(line, "share")) cmd_share(arg);`

`COMMANDS[]`: add `"share",` before `"taskcost"`.

`cmd_help`: change the `radios` lines to

```c
  con_write("radios   wifi [scan|SSID PASS|saved|forget|off]\n");
  con_write("         mouse, get URL, share [off|log] (card as a drive)\n");
```

- [ ] **Step 2: Implement in `shellcmd.c`**

Add `#include "kernel/net/share.h"` with the other includes, and append:

```c
void cmd_share(const char *arg) {
  const char *l;

  if (arg && !strcmp(arg, "off")) {
    if (!share_running()) { con_write("not sharing\n"); return; }
    share_stop();
    con_write("stopped\n");
    return;
  }
  if (arg && !strcmp(arg, "log")) {
    int n = 0;
    while ((l = share_take_log()) != NULL) { con_printf("  %s\n", l); n++; }
    if (!n) con_write("nothing since last time\n");
    return;
  }
  if (arg && *arg) { con_write("usage: share [off|log]\n"); return; }

  if (share_running()) {
    con_printf("sharing at %s\n", share_url());
    return;
  }
  if (share_start(0) != 0) { err("share", share_error()); return; }
  con_printf("sharing the card at %s\n", share_url());
  con_write("windows: map network drive, or  net use X: ");
  con_printf("%s\n", share_url());
  con_write("mac: finder, go, connect to server. no password.\n");
  con_write("anyone on this network can read and write the card. share off ends it.\n");
}
```

(`err` is the static helper already used by `cmd_update` in this file.)

- [ ] **Step 3: Build and try it on the device**

Run: `python tools/build_apps.py` then `python -m platformio run -t upload --upload-port COM3`.
On the device console: `wifi saved` (if not already joined), then `share`. Expected: the URL line. On the PC: `curl -i -X OPTIONS http://<ip>/` shows `DAV: 1,2`; `curl -i -X PROPFIND -H "Depth: 1" http://<ip>/desktop/` shows a 207 with one `<D:response>` per app. `share log` on the device lists both. `share off` stops; `curl` then fails to connect.

- [ ] **Step 4: Commit**

```bash
rtk git add src/shellcmd.h src/shellcmd.c src/main.c
rtk git commit -m "console: share, share off, share log"
```

---

### Task 8: API version 23 and the Share app

**Files:**
- Modify: `kernel/app/capp.h:41` (version), `:455-459` (append four entries before the closing `} CardApi;`)
- Modify: `kernel/app/cardapi.c:309-333` (four wrappers, four table entries)
- Create: `apps/share.c`
- Modify: `tools/build_apps.py:124-136` (FOLDERS: `"share": "Net"`)
- Modify: `CLAUDE.md` — the "API version" paragraph and a short "Share" paragraph

**Interfaces:**
- Produces, in `CardApi` (appended, in this order):

```c
  /* ---- the card as a network drive ----
   *
   * WebDAV on port 80 while it is on; see kernel/net/share.h. share_start
   * returns 0 or -1, and share_status says either the URL to type on the PC
   * or why it could not start. The share stops by itself when the app that
   * started it is closed. share_take_log hands back one line per call --
   * "PUT /desktop/x.capp" -- or NULL, for an app that shows what is going
   * on; poll it from tick. */
  int         (*share_start)(void);
  void        (*share_stop)(void);
  const char *(*share_status)(void);
  const char *(*share_take_log)(void);
```

- [ ] **Step 1: Bump the version and append the entries**

`capp.h`: `#define CAPP_API_VERSION 23`. Append the block above after `http_poll` inside the `CardApi` struct.

`cardapi.c`: add `#include "kernel/net/share.h"`, then before `static const CardApi API`:

```c
static int api_share_start(void) { return share_start(1); }
static void api_share_stop(void) { share_stop(); }
static const char *api_share_status(void) {
  return share_running() ? share_url() : share_error();
}
static const char *api_share_take_log(void) { return share_take_log(); }
```

and append to the table after `api_http_start, api_http_poll,`:

```c
  api_share_start, api_share_stop, api_share_status, api_share_take_log,
```

- [ ] **Step 2: Write the app**

`apps/share.c`:

```c
/* Share -- the card as a network drive, while this is open.
 *
 * The server is the kernel's (kernel/net/share.c); this is the switch and
 * the readout. It starts sharing when it opens and the kernel stops sharing
 * when it is closed, which is the whole design: a thing you can see is on.
 * The console's `share` command outlives the command instead, for the
 * development loop.
 */

#include "kernel/app/capp.h"

#define LINES     7
#define LINE_MAX  40

#define CLR_BG   CAPP_RGB(8, 10, 14)
#define CLR_FG   CAPP_RGB(226, 232, 242)
#define CLR_DIM  CAPP_RGB(130, 140, 158)
#define CLR_URL  CAPP_RGB(120, 220, 140)
#define CLR_BAD  CAPP_RGB(240, 120, 100)

static const CardApi *api;

static struct {
  int  on;
  char status[96];
  char log[LINES][LINE_MAX];
  int  nlog;                 /* lines used; the newest is at nlog - 1 */
  uint32_t count;
} S;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void start(void) {
  S.on = api->share_start() == 0;
  api->fmt(S.status, sizeof S.status, "%s", api->share_status());
}

static void app_paint(void *st, CRect c) {
  int i, y;
  (void)st;
  api->fill(c, CLR_BG);
  api->text((short)(c.x + 8), (short)(c.y + 6), "Share", CLR_FG, CLR_BG);
  if (S.on) {
    api->text((short)(c.x + 8), (short)(c.y + 20), "the card is on the network at",
              CLR_DIM, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 32), S.status, CLR_URL, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 46),
              "map it as a drive on the pc; escape stops", CLR_DIM, CLR_BG);
  } else {
    api->text((short)(c.x + 8), (short)(c.y + 20), "not sharing:", CLR_DIM, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 32), S.status, CLR_BAD, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 46), "enter to try again", CLR_DIM, CLR_BG);
  }
  y = c.y + 62;
  for (i = 0; i < S.nlog; i++, y += 10)
    api->text((short)(c.x + 8), (short)y, S.log[i], CLR_FG, CLR_BG);
  {
    char bar[48];
    api->fmt(bar, sizeof bar, "%lu requests", (unsigned long)S.count);
    api->text((short)(c.x + 8), (short)(c.y + c.h - 10), bar, CLR_DIM, CLR_BG);
  }
}

static int app_tick(void *st, uint32_t now) {
  const char *l;
  int changed = 0;
  (void)st; (void)now;
  while ((l = api->share_take_log()) != NULL) {
    if (S.nlog == LINES) {
      api->mem_move(S.log[0], S.log[1], sizeof S.log - sizeof S.log[0]);
      S.nlog--;
    }
    api->fmt(S.log[S.nlog++], LINE_MAX, "%s", l);
    S.count++;
    changed = 1;
  }
  return changed;
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if ((k == CAPP_KEY_ENTER || k == ' ') && !S.on) { start(); return 1; }
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_NET,
  "Share",
  /* 16x16: a card with a wave leaving it. */
  { 0x00, 0x00, 0x3F, 0xC0, 0x20, 0x40, 0x2F, 0x40,
    0x20, 0x40, 0x2F, 0x40, 0x20, 0x40, 0x2F, 0x48,
    0x20, 0x44, 0x20, 0x52, 0x20, 0x4A, 0x20, 0x4A,
    0x3F, 0xD2, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00 },
  "escape\tstop sharing and leave\n"
  "enter\ttry again after a failure\n"
  "\n"
  "on the pc: map network drive to the url shown, no password.\n"
  "windows refuses files over 50 MB by default (WebClient\n"
  "FileSizeLimitInBytes in the registry). copying a folder\n"
  "within the drive is not supported; copy its files.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&S, 0, sizeof S);
  start();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  api->ui(&UI);
  return 0;
}
```

`api->mem_set`, `api->mem_move` and `api->fmt` are the field names in `capp.h` (checked).

- [ ] **Step 3: Seed it into the Net folder and update CLAUDE.md**

`tools/build_apps.py` FOLDERS: add `"share":    "Net",` under `"screen"`.

`CLAUDE.md`: in the paragraph beginning `**API version 17.**` change the number to 23 and append: `share_start`/`share_stop`/`share_status`/`share_take_log` (23) to the list of arrivals. Add after the voice paragraph:

```
**The card is a network drive when you ask.** `share` in the console, or the
Share app, runs a WebDAV server on port 80 (`kernel/net/share.c` is the
socket and task; `kernel/net/dav.c` is the protocol and is host-tested).
Windows maps `http://IP/` as a drive letter, Finder connects to it. No
password: the boundary is the LAN, so it is a thing you turn on. Design in
`docs/superpowers/specs/2026-09-14-webdav-share-design.md`.
```

- [ ] **Step 4: Build, flash, try**

Run: `python tools/build_apps.py` (must report share.capp built and refuse nothing), then `python -m platformio run -t upload --upload-port COM3`.
On the device: `update apps` is not needed — the blob is embedded, but an existing card already has `/desktop` so first-boot seeding will not re-run. Either copy `build/apps/share.capp` onto the card by hand or, with the console share from Task 7 running, `curl -T build/apps/share.capp http://<ip>/desktop/Net/share.capp` (confirm the built path with `ls build/apps` or wherever `build_apps.py` says it writes). Open Net → Share in the launcher: the URL shows, requests scroll as the PC asks. Escape: `share` in the console then says `not sharing`.

- [ ] **Step 5: Commit**

```bash
rtk git add kernel/app/capp.h kernel/app/cardapi.c apps/share.c tools/build_apps.py CLAUDE.md
rtk git commit -m "API 23: share_*; the Share app

Four calls over kernel/net/share.c. The app is the switch and the readout;
closing it stops the share."
```

---

### Task 9: The client test, and the numbers

**Files:**
- Create: `tools/test_share.py`

**Interfaces:**
- Consumes: a running share at `http://<ip>/` (Task 7 or 8).

- [ ] **Step 1: Write the test**

```python
#!/usr/bin/env python3
"""Exercise the device's WebDAV share from the PC.

    python tools/test_share.py 192.168.1.23

Every method the server claims, a 1 MB round trip compared byte for byte,
a traversal attempt that must be refused, and the transfer rates -- which
belong in the commit message, per the working agreements.
"""
import http.client
import os
import sys
import time

MB = 1024 * 1024
TEST_DIR = "/sharetest"


class Dav:
    def __init__(self, host):
        self.host = host

    def req(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection(self.host, 80, timeout=30)
        h = dict(headers or {})
        if body is not None:
            h["Content-Length"] = str(len(body))
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, dict(r.getheaders()), data


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        check.fails += 1


check.fails = 0


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = Dav(sys.argv[1])

    st, h, _ = d.req("OPTIONS", "/")
    check(st == 200 and "1,2" in h.get("DAV", ""), "OPTIONS advertises DAV: 1,2")
    check("MS-Author-Via" in h, "OPTIONS has MS-Author-Via")

    st, _, body = d.req("PROPFIND", "/", headers={"Depth": "0"})
    check(st == 207 and b"<D:collection/>" in body, "PROPFIND / depth 0 is a collection")
    st, _, body = d.req("PROPFIND", "/desktop", headers={"Depth": "1"})
    check(st == 207 and body.count(b"<D:response>") > 1, "PROPFIND /desktop depth 1 lists children")
    st, _, _ = d.req("PROPFIND", "/", headers={"Depth": "infinity"})
    check(st == 403, "Depth: infinity is refused")

    d.req("DELETE", TEST_DIR)                       # from a previous run, if any
    st, _, _ = d.req("MKCOL", TEST_DIR)
    check(st == 201, "MKCOL creates a folder")
    st, _, _ = d.req("MKCOL", TEST_DIR)
    check(st == 405, "MKCOL on an existing folder is 405")
    st, _, _ = d.req("MKCOL", "/nope/deeper")
    check(st == 409, "MKCOL without a parent is 409")

    blob = os.urandom(MB)
    t = time.time()
    st, _, _ = d.req("PUT", TEST_DIR + "/one.bin", body=blob)
    up = MB / (time.time() - t) / 1024
    check(st == 201, "PUT creates (%d KB/s up)" % up)
    st, _, _ = d.req("PUT", TEST_DIR + "/one.bin", body=blob[:10])
    check(st == 204, "PUT over an existing file is 204")
    st, _, _ = d.req("PUT", TEST_DIR + "/one.bin", body=blob)
    check(st == 204, "PUT restores it")

    t = time.time()
    st, h, got = d.req("GET", TEST_DIR + "/one.bin")
    down = MB / (time.time() - t) / 1024
    check(st == 200 and got == blob, "GET returns the same bytes (%d KB/s down)" % down)
    check(h.get("Content-Length") == str(MB), "GET has Content-Length")
    st, h, got = d.req("HEAD", TEST_DIR + "/one.bin")
    check(st == 200 and got == b"" and h.get("Content-Length") == str(MB), "HEAD has the length, no body")

    st, _, _ = d.req("MOVE", TEST_DIR + "/one.bin",
                     headers={"Destination": "http://%s%s/two.bin" % (d.host, TEST_DIR)})
    check(st == 201, "MOVE renames")
    st, _, _ = d.req("GET", TEST_DIR + "/one.bin")
    check(st == 404, "the old name is gone")
    st, _, _ = d.req("COPY", TEST_DIR + "/two.bin",
                     headers={"Destination": "http://%s%s/three.bin" % (d.host, TEST_DIR)})
    check(st == 201, "COPY copies a file")
    st, _, _ = d.req("COPY", TEST_DIR + "/two.bin",
                     headers={"Destination": "http://%s%s/three.bin" % (d.host, TEST_DIR),
                              "Overwrite": "F"})
    check(st == 412, "COPY with Overwrite: F onto an existing file is 412")
    st, _, _ = d.req("COPY", TEST_DIR, headers={"Destination": "http://%s/sharetest2" % d.host})
    check(st == 403, "COPY of a folder is the documented 403")

    st, _, body = d.req("PROPPATCH", TEST_DIR + "/two.bin", body=b"<x/>")
    check(st == 207 and b"200 OK" in body, "PROPPATCH says yes")
    st, h, body = d.req("LOCK", TEST_DIR + "/two.bin", body=b"<x/>")
    check(st == 200 and "Lock-Token" in h and b"locktoken" in body, "LOCK hands out a token")
    st, _, _ = d.req("UNLOCK", TEST_DIR + "/two.bin", headers={"Lock-Token": h.get("Lock-Token", "")})
    check(st == 204, "UNLOCK is 204")

    st, _, _ = d.req("GET", "/../etc")
    check(st in (403, 400), "traversal is refused (%d)" % st)
    st, _, _ = d.req("GET", "/%2e%2e/x")
    check(st == 403, "encoded traversal is refused")
    st, _, _ = d.req("BREW", "/")
    check(st == 405, "an unknown method is 405")
    st, _, _ = d.req("GET", "/does-not-exist")
    check(st == 404, "a missing file is 404")

    st, _, _ = d.req("DELETE", TEST_DIR)
    check(st == 204, "DELETE removes the folder and its files")
    st, _, _ = d.req("PROPFIND", TEST_DIR, headers={"Depth": "0"})
    check(st == 404, "and it is gone")

    print("\n%d failures; up %d KB/s, down %d KB/s" % (check.fails, up, down))
    sys.exit(1 if check.fails else 0)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it against the device**

Start the share on the device (`share` in the console). Run: `python tools/test_share.py <ip>`.
Expected: `0 failures` and two rates. If Python's `http.client` complains about the `BREW` method, that is fine — it sends anything. If `MOVE` fails with 409, check that `Destination` decoding strips `http://host` (Task 3's test covered it; the device path is `dav_parse` → `dav_decode_path`).

- [ ] **Step 3: Map it in Explorer**

On the PC: Explorer → This PC → Map network drive → folder `http://<ip>/` → Finish (the WebClient service must be running; `net start webclient` if the dialog says the folder is invalid). Expected: the drive opens showing `desktop`, `cache`, etc.; copying a `.capp` into `desktop` shows up in the launcher on its next visit; opening a `.txt` from the drive works; deleting a file works. Note anything that did not.

- [ ] **Step 4: Measure memory**

On the device with the share on and Explorer connected: `mem`. Record free heap and low water; compare with the share off. Expected: roughly 15–20 KB less while on.

- [ ] **Step 5: Commit with the numbers**

```bash
rtk git add tools/test_share.py
rtk git commit -m "tools/test_share.py: every method, a 1 MB round trip, the rates

Measured on the device: up N KB/s, down N KB/s over WiFi to SD; heap N KB
free with the share on against N KB off. Explorer on Windows 11 Home maps
it and copies both ways."
```

Fill in the Ns from Steps 2 and 4 — a commit message with `N` in it is not finished.

---

## Self-review

**Spec coverage.** Port 80, root `/`, no auth — Task 6. One connection, keep-alive, backlog 4 — Task 6 (`BACKLOG`, `serve_one` loop, `IDLE_MS`). Own task, created/deleted with the share, never draws, log queue — Task 6. `fs_fat.c` lock and `mtime` — Task 1 (plus the `FsDir` prefix the spec did not know it needed). Percent-coded UTF-8 hrefs and traversal → 403 — Task 3. PROPFIND depth 0/1, infinity → 403, seven properties, streamed per entry — Tasks 5 and 6 (chunked 207). PROPPATCH says yes — Tasks 5/6. GET/PUT through 4 KB, chunked PUT → 411, 100-continue, Range ignored, 507 on a failed write — Task 6. HEAD, MKCOL, DELETE recursive, MOVE, file COPY, folder COPY → 403, 405 with Allow — Task 6. 50 MB limit documented — Task 8 help text. No discovery, URL shown — Tasks 7/8. Console `share`/`share off` outliving the command, app stopping on close — Tasks 7, 6 (`share_app_closed`), 8. API 23, `build_apps.py` — Task 8. 40 KB memory guard in words — Task 6. Host tests for parsing, split headers (`dav_headers_end` on a partial buffer), traversal shapes, Depth, PROPFIND fixtures for file/folder, dates, lock token, every method — Tasks 2–5. Device test with rates and the Explorer check — Task 9.

**Gaps closed inline.** The spec's "RFC 1123 dates on and off DST" is moot since the formatter takes UTC and emits UTC; the tests cover the RFC example, the epoch and a leap day instead. A body arriving in the same packet as the headers is handled by reducing the drain count (Task 6, `serve_one`) — it matters for `PROPFIND` from Windows, which sends a small XML body.

**Type consistency.** `dav_decode_path(const char*, size_t, char*, size_t)` is used with the same signature in Tasks 2, 3 and 6. `DavEntry` fields `{path, size, is_dir, mtime}` match between Task 5 and Task 6's `propfind_one`. `share_start(int)` in the kernel vs `share_start(void)` in the API is deliberate (`api_share_start` passes 1). `FsStat.mtime` from Task 1 is read in Task 6.
