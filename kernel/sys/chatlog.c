/* The conversation on the card. See chatlog.h for why it is a file. */

#include "kernel/sys/chatlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY   "history.json"
#define HISTORY_T "history.tmp"
#define PATH_MAX_ 128
#define WINDOW    512

static const ChatlogOps *s_ops;
static char s_dir[64];

static const char *full(const char *name) {
  static char path[PATH_MAX_];
  snprintf(path, sizeof path, "%s/%s", s_dir, name);
  return path;
}

void chatlog_init(const ChatlogOps *ops, const char *dir) {
  s_ops = ops;
  snprintf(s_dir, sizeof s_dir, "%s", dir);
}

void chatlog_clear(void) {
  if (!s_ops) return;
  s_ops->remove(full(HISTORY));
  s_ops->remove(full(HISTORY_T));
}

static int file_size(const char *name) {
  int fd, n;
  if (!s_ops) return 0;
  fd = s_ops->open(full(name), CHATLOG_O_READ);
  if (fd < 0) return 0;
  n = s_ops->seek(fd, 0, CHATLOG_SEEK_END);
  s_ops->close(fd);
  return n < 0 ? 0 : n;
}

int chatlog_size(void) { return file_size(HISTORY); }

/* ---- writing --------------------------------------------------------------- */

static int w_str(int fd, const char *s) {
  size_t n = strlen(s);
  return (size_t)s_ops->write(fd, s, n) == n ? 0 : -1;
}

/* A JSON string body, escaped as the grammar requires and no more. */
static int w_json(int fd, const char *s) {
  char buf[8];
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    const char *out = NULL;
    switch (c) {
    case '"':  out = "\\\""; break;
    case '\\': out = "\\\\"; break;
    case '\n': out = "\\n";  break;
    case '\r': out = "\\r";  break;
    case '\t': out = "\\t";  break;
    default:
      if (c < 0x20) { snprintf(buf, sizeof buf, "\\u%04x", c); out = buf; }
      break;
    }
    if (out) { if (w_str(fd, out) != 0) return -1; }
    else if (s_ops->write(fd, s, 1) != 1) return -1;
  }
  return 0;
}

/* Open the history for one more message, writing the comma that separates
 * it from the last one. */
static int open_for_turn(void) {
  int had = chatlog_size() > 0;
  int fd = s_ops->open(full(HISTORY), CHATLOG_O_WRITE | CHATLOG_O_CREATE | CHATLOG_O_APPEND);
  if (fd < 0) return -1;
  if (had && w_str(fd, ",") != 0) { s_ops->close(fd); return -1; }
  return fd;
}

int chatlog_add_user(const char *text) {
  int fd, rc;
  if (!s_ops || !text) return -1;
  fd = open_for_turn();
  if (fd < 0) return -1;
  rc = w_str(fd, "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"");
  if (rc == 0) rc = w_json(fd, text);
  if (rc == 0) rc = w_str(fd, "\"}]}");
  s_ops->close(fd);
  return rc;
}

int chatlog_add_tool_results(const ChatToolResult *r, int n) {
  int fd, rc, i;
  if (!s_ops || !r || n <= 0) return -1;
  fd = open_for_turn();
  if (fd < 0) return -1;
  rc = w_str(fd, "{\"role\":\"user\",\"content\":[");
  for (i = 0; i < n && rc == 0; i++) {
    if (i) rc = w_str(fd, ",");
    if (rc == 0) rc = w_str(fd, "{\"type\":\"tool_result\",\"tool_use_id\":\"");
    if (rc == 0) rc = w_json(fd, r[i].id);
    if (rc == 0) rc = w_str(fd, "\",\"content\":\"");
    if (rc == 0) rc = w_json(fd, r[i].text);
    if (rc == 0) rc = w_str(fd, "\"}");
  }
  if (rc == 0) rc = w_str(fd, "]}");
  s_ops->close(fd);
  return rc;
}

/* Copy [from, to) of one file onto the end of another, through the window. */
static int copy_range(int src, int32_t from, int32_t to, int dst) {
  char buf[WINDOW];
  if (s_ops->seek(src, from, CHATLOG_SEEK_SET) != from) return -1;
  while (from < to) {
    int want = (int)(to - from);
    int n;
    if (want > (int)sizeof buf) want = (int)sizeof buf;
    n = s_ops->read(src, buf, (size_t)want);
    if (n <= 0) return -1;
    if (s_ops->write(dst, buf, (size_t)n) != n) return -1;
    from += n;
  }
  return 0;
}

int chatlog_write_request(const char *request_file, const char *head, const char *tail) {
  int src, dst, size, rc = 0;
  if (!s_ops) return -1;
  size = chatlog_size();
  dst = s_ops->open(full(request_file), CHATLOG_O_WRITE | CHATLOG_O_CREATE | CHATLOG_O_TRUNC);
  if (dst < 0) return -1;
  if (w_str(dst, head) != 0) rc = -1;
  if (rc == 0 && size > 0) {
    src = s_ops->open(full(HISTORY), CHATLOG_O_READ);
    if (src < 0) rc = -1;
    else { rc = copy_range(src, 0, size, dst); s_ops->close(src); }
  }
  if (rc == 0) rc = w_str(dst, tail);
  s_ops->close(dst);
  return rc;
}

/* ---- reading through a window --------------------------------------------- */

typedef struct {
  int     fd;
  char    buf[WINDOW];
  int     len, pos;
  int32_t base;         /* file offset of buf[0] */
  int     unread;       /* one character of lookahead, or -1 */
} Reader;

static void rd_open(Reader *r, int fd) {
  r->fd = fd; r->len = r->pos = 0; r->base = 0; r->unread = -1;
}

static int rd_getc(Reader *r) {
  int c;
  if (r->unread >= 0) { c = r->unread; r->unread = -1; return c; }
  if (r->pos >= r->len) {
    int n = s_ops->read(r->fd, r->buf, sizeof r->buf);
    if (n <= 0) return -1;
    r->base += r->len;
    r->len = n; r->pos = 0;
  }
  return (unsigned char)r->buf[r->pos++];
}

static void rd_ungetc(Reader *r, int c) { r->unread = c; }

/* Offset of the next character rd_getc would return. */
static int32_t rd_tell(const Reader *r) {
  return r->base + r->pos - (r->unread >= 0 ? 1 : 0);
}

static int rd_skip_ws(Reader *r) {
  int c;
  do c = rd_getc(r); while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
  return c;
}

/* Expect `want` as the next non-space character. */
static int rd_expect(Reader *r, int want) {
  return rd_skip_ws(r) == want ? 0 : -1;
}

static void put_utf8(char *out, size_t cap, size_t *n, unsigned cp) {
  unsigned char b[4];
  int len;
  if (cp < 0x80) { b[0] = (unsigned char)cp; len = 1; }
  else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); len = 2; }
  else if (cp < 0x10000) {
    b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (unsigned char)(0x80 | (cp & 0x3F)); len = 3;
  } else {
    b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); len = 4;
  }
  if (out && *n + (size_t)len < cap) { memcpy(out + *n, b, (size_t)len); *n += (size_t)len; }
}

static int hex4(Reader *r) {
  int i, v = 0;
  for (i = 0; i < 4; i++) {
    int c = rd_getc(r);
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return -1;
  }
  return v;
}

/* The body of a string, the opening quote already consumed; unescaped into
 * `out` (NULL to discard), truncated to fit. Returns 0 at the closing quote. */
static int rd_string(Reader *r, char *out, size_t cap, size_t *len) {
  size_t n = len ? *len : 0;
  for (;;) {
    int c = rd_getc(r);
    if (c < 0) return -1;
    if (c == '"') break;
    if (c == '\\') {
      c = rd_getc(r);
      switch (c) {
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case 'u': {
        int cp = hex4(r);
        if (cp < 0) return -1;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          /* A surrogate pair: the low half follows as another \\u escape. */
          int lo = -1;
          if (rd_getc(r) == '\\' && rd_getc(r) == 'u') lo = hex4(r);
          cp = (lo >= 0xDC00 && lo <= 0xDFFF) ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
        }
        put_utf8(out, cap, &n, (unsigned)cp);
        continue;
      }
      default: break;               /* \" \\ \/ and anything else: itself */
      }
    }
    if (out && n + 1 < cap) out[n++] = (char)c;
  }
  if (out && cap) out[n] = 0;
  if (len) *len = n;
  return 0;
}

static int rd_skip_value(Reader *r);

/* An object or array, its opening bracket already consumed. */
static int rd_skip_container(Reader *r, int close) {
  for (;;) {
    int c = rd_skip_ws(r);
    if (c < 0) return -1;
    if (c == close) return 0;
    if (c == ',') continue;
    if (c == '"') { if (rd_string(r, NULL, 0, NULL) != 0) return -1; continue; }
    if (c == ':') continue;
    rd_ungetc(r, c);
    if (rd_skip_value(r) != 0) return -1;
  }
}

/* A bare token: number, true, false, null. Copied into `out` if given. */
static int rd_token(Reader *r, char *out, size_t cap) {
  size_t n = 0;
  for (;;) {
    int c = rd_getc(r);
    if (c < 0) break;
    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      rd_ungetc(r, c);
      break;
    }
    if (out && n + 1 < cap) out[n++] = (char)c;
  }
  if (out && cap) out[n] = 0;
  return 0;
}

static int rd_skip_value(Reader *r) {
  int c = rd_skip_ws(r);
  if (c < 0) return -1;
  if (c == '"') return rd_string(r, NULL, 0, NULL);
  if (c == '{') return rd_skip_container(r, '}');
  if (c == '[') return rd_skip_container(r, ']');
  rd_ungetc(r, c);
  return rd_token(r, NULL, 0);
}

/* The next key of an object, or -1 at its end. `c` is the character after
 * the previous value: ',' or '}' or the '{' that opened it. */
static int rd_key(Reader *r, char *key, size_t cap) {
  int c = rd_skip_ws(r);
  if (c == ',') c = rd_skip_ws(r);
  if (c != '"') return -1;                  /* '}' or EOF */
  if (rd_string(r, key, cap, NULL) != 0) return -1;
  return rd_expect(r, ':');
}

/* ---- the reply ------------------------------------------------------------- */

typedef struct {
  ChatReply    *out;
  size_t        textlen;
  int32_t       content_from, content_to;   /* the array, brackets included */
} Scan;

static ChatStop stop_of(const char *s) {
  if (!strcmp(s, "end_turn")) return CHAT_STOP_END;
  if (!strcmp(s, "tool_use")) return CHAT_STOP_TOOL;
  if (!strcmp(s, "max_tokens")) return CHAT_STOP_MAX;
  return CHAT_STOP_OTHER;
}

/* One content block, its '{' consumed. */
static int scan_block(Reader *r, Scan *s) {
  char key[24], type[16] = "";
  ChatToolCall tc;
  size_t text_before = s->textlen;
  memset(&tc, 0, sizeof tc);

  while (rd_key(r, key, sizeof key) == 0) {
    if (!strcmp(key, "type")) {
      if (rd_expect(r, '"') != 0 || rd_string(r, type, sizeof type, NULL) != 0) return -1;
    } else if (!strcmp(key, "text")) {
      if (rd_expect(r, '"') != 0) return -1;
      if (rd_string(r, s->out->text, sizeof s->out->text, &s->textlen) != 0) return -1;
    } else if (!strcmp(key, "id")) {
      if (rd_expect(r, '"') != 0 || rd_string(r, tc.id, sizeof tc.id, NULL) != 0) return -1;
    } else if (!strcmp(key, "name")) {
      if (rd_expect(r, '"') != 0 || rd_string(r, tc.name, sizeof tc.name, NULL) != 0) return -1;
    } else if (!strcmp(key, "input")) {
      /* The first value is the argument; the tools here take one each. */
      char ikey[24];
      if (rd_expect(r, '{') != 0) return -1;
      if (rd_key(r, ikey, sizeof ikey) == 0) {
        int c = rd_skip_ws(r);
        if (c == '"') { if (rd_string(r, tc.arg, sizeof tc.arg, NULL) != 0) return -1; }
        else { rd_ungetc(r, c); rd_token(r, tc.arg, sizeof tc.arg); }
        if (!strcmp(tc.arg, "true")) tc.num = 1;
        else if (!strcmp(tc.arg, "false")) tc.num = 0;
        else tc.num = atoi(tc.arg);
        if (rd_skip_container(r, '}') != 0) return -1;   /* the rest of input */
      }
    } else {
      if (rd_skip_value(r) != 0) return -1;
    }
  }

  if (!strcmp(type, "tool_use")) {
    if (s->out->ntools < CHAT_TOOLS_MAX) s->out->tool[s->out->ntools++] = tc;
  } else if (strcmp(type, "text") != 0) {
    /* Not text: whatever landed in the text buffer from this block goes. */
    s->textlen = text_before;
    s->out->text[s->textlen] = 0;
  }
  return 0;
}

static int scan_message(Reader *r, Scan *s) {
  char key[24];
  if (rd_expect(r, '{') != 0) return -1;
  while (rd_key(r, key, sizeof key) == 0) {
    if (!strcmp(key, "content")) {
      int c;
      if (rd_expect(r, '[') != 0) return -1;
      s->content_from = rd_tell(r) - 1;
      for (;;) {
        c = rd_skip_ws(r);
        if (c == ']') break;
        if (c == ',') continue;
        if (c != '{') return -1;
        if (scan_block(r, s) != 0) return -1;
      }
      s->content_to = rd_tell(r);
    } else if (!strcmp(key, "stop_reason")) {
      char v[24];
      int c = rd_skip_ws(r);
      if (c == '"') { if (rd_string(r, v, sizeof v, NULL) != 0) return -1; s->out->stop = stop_of(v); }
      else { rd_ungetc(r, c); rd_token(r, NULL, 0); }      /* null */
    } else if (!strcmp(key, "error")) {
      /* "type: message", because the message is sometimes just "Error" and
       * the type -- rate_limit_error, authentication_error -- is then the
       * only clue there is. */
      char ekey[24], etype[40] = "", emsg[200] = "";
      if (rd_expect(r, '{') != 0) return -1;
      s->out->error = 1;
      while (rd_key(r, ekey, sizeof ekey) == 0) {
        if (!strcmp(ekey, "message")) {
          if (rd_expect(r, '"') != 0) return -1;
          if (rd_string(r, emsg, sizeof emsg, NULL) != 0) return -1;
        } else if (!strcmp(ekey, "type")) {
          if (rd_expect(r, '"') != 0) return -1;
          if (rd_string(r, etype, sizeof etype, NULL) != 0) return -1;
        } else if (rd_skip_value(r) != 0) return -1;
      }
      snprintf(s->out->text, sizeof s->out->text, "%s%s%s",
               etype, etype[0] && emsg[0] ? ": " : "", emsg);
      s->textlen = strlen(s->out->text);
    } else {
      if (rd_skip_value(r) != 0) return -1;
    }
  }
  return 0;
}

static int scan_file(const char *name, Scan *s) {
  Reader r;
  int fd, rc;
  fd = s_ops->open(full(name), CHATLOG_O_READ);
  if (fd < 0) return -1;
  rd_open(&r, fd);
  rc = scan_message(&r, s);
  s_ops->close(fd);
  return rc;
}

int chatlog_scan_reply(const char *reply_file, ChatReply *out) {
  Scan s;
  if (!s_ops || !out) return -1;
  memset(out, 0, sizeof *out);
  out->stop = CHAT_STOP_OTHER;
  memset(&s, 0, sizeof s);
  s.out = out;
  s.content_from = s.content_to = -1;
  if (scan_file(reply_file, &s) != 0) return -1;
  out->text[s.textlen] = 0;
  return 0;
}

int chatlog_add_reply(const char *reply_file) {
  Scan s;
  ChatReply tmp;
  int src, dst, rc;
  if (!s_ops) return -1;
  memset(&s, 0, sizeof s);
  memset(&tmp, 0, sizeof tmp);             /* the scan counts tools into it */
  s.out = &tmp;
  s.content_from = s.content_to = -1;
  if (scan_file(reply_file, &s) != 0 || s.content_from < 0 || s.content_to <= s.content_from)
    return -1;

  dst = open_for_turn();
  if (dst < 0) return -1;
  src = s_ops->open(full(reply_file), CHATLOG_O_READ);
  if (src < 0) { s_ops->close(dst); return -1; }
  rc = w_str(dst, "{\"role\":\"assistant\",\"content\":");
  if (rc == 0) rc = copy_range(src, s.content_from, s.content_to, dst);
  if (rc == 0) rc = w_str(dst, "}");
  s_ops->close(src);
  s_ops->close(dst);
  return rc;
}

/* ---- trimming --------------------------------------------------------------- */

static const char USER_TEXT_PREFIX[] = "{\"role\":\"user\",\"content\":[{\"type\":\"text\"";

/* Where the message starting at `from` ends: the offset just past its '}'. */
static int32_t message_end(int fd, int32_t from) {
  Reader r;
  if (s_ops->seek(fd, from, CHATLOG_SEEK_SET) != from) return -1;
  rd_open(&r, fd);
  r.base = from;
  if (rd_skip_value(&r) != 0) return -1;
  return rd_tell(&r);
}

static int starts_with_user_text(int fd, int32_t at) {
  char buf[sizeof USER_TEXT_PREFIX];
  int n;
  if (s_ops->seek(fd, at, CHATLOG_SEEK_SET) != at) return 0;
  n = s_ops->read(fd, buf, sizeof buf - 1);
  if (n < (int)sizeof buf - 1) return 0;
  return memcmp(buf, USER_TEXT_PREFIX, sizeof buf - 1) == 0;
}

int chatlog_trim(int cap) {
  int fd, dst, size, rc;
  int32_t from = 0;
  if (!s_ops) return -1;
  size = chatlog_size();
  if (size <= cap) return 0;

  fd = s_ops->open(full(HISTORY), CHATLOG_O_READ);
  if (fd < 0) return -1;
  while (size - from > cap && from < size) {
    /* Drop the first message, then keep dropping until what is first is a
     * user text turn -- so an assistant turn never opens the history and a
     * tool_result never stands without its tool_use. */
    do {
      int32_t end = message_end(fd, from);
      if (end < 0) { s_ops->close(fd); return -1; }
      from = end;
      if (from < size) from++;                  /* the comma */
    } while (from < size && !starts_with_user_text(fd, from));
  }

  if (from >= size) {
    s_ops->close(fd);
    chatlog_clear();
    return 0;
  }
  dst = s_ops->open(full(HISTORY_T), CHATLOG_O_WRITE | CHATLOG_O_CREATE | CHATLOG_O_TRUNC);
  if (dst < 0) { s_ops->close(fd); return -1; }
  rc = copy_range(fd, from, size, dst);
  s_ops->close(dst);
  s_ops->close(fd);
  if (rc != 0) return -1;
  {
    /* full() has one buffer, so the second name is copied out first. */
    char tmp[PATH_MAX_];
    snprintf(tmp, sizeof tmp, "%s", full(HISTORY_T));
    return s_ops->rename(tmp, full(HISTORY)) == 0 ? 0 : -1;
  }
}
