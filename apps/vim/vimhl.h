/* Vim's syntax colours: one line at a time, with a state carried over.
 *
 * hl_line colours one line and returns the state its end leaves -- inside a
 * C block comment, inside a Python triple-quoted string, inside a Markdown
 * fence, or nothing. That byte is all a line needs from the lines above it,
 * so the editor keeps it at a checkpoint every HL_STEP lines (vimcore.h) and
 * never rescans the file to paint a screen: the state at the top row is the
 * nearest checkpoint plus at most HL_STEP - 1 lines.
 *
 * Not a parser: tokens, in one pass. A word is a keyword or not, a number
 * starts with a digit, a string runs to its closing quote or the end of
 * the line (a C string that does not close is not carried on).
 *
 * Portable; host-tested in apps/vim/test/test_vim.c.
 */
#ifndef CARDOS_VIMHL_H
#define CARDOS_VIMHL_H

#include "apps/vim/vimbuf.h"

enum { HL_N = 0, HL_KW, HL_TYPE, HL_STR, HL_COM, HL_NUM, HL_PRE, HL_HEAD,
       HL_CODE, HL_EMPH, HL_COUNT };

enum { LANG_PLAIN = 0, LANG_C, LANG_PY, LANG_MD };

/* States a line can end in. */
enum { HS_NONE = 0, HS_CCOM, HS_PY3D, HS_PY3S, HS_MDFENCE };

static const char *const HL_C_KW[] = {
  "if", "else", "for", "while", "do", "switch", "case", "default", "break",
  "continue", "return", "goto", "sizeof", "typedef", "struct", "union",
  "enum", "static", "const", "extern", "volatile", "register", "inline",
  "NULL", 0
};
static const char *const HL_C_TYPE[] = {
  "void", "char", "short", "int", "long", "float", "double", "signed",
  "unsigned", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
  "int8_t", "int16_t", "int32_t", "int64_t", "bool", "CRect", "CardApi", 0
};
static const char *const HL_PY_KW[] = {
  "and", "as", "assert", "async", "await", "break", "class", "continue",
  "def", "del", "elif", "else", "except", "finally", "for", "from",
  "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or",
  "pass", "raise", "return", "try", "while", "with", "yield", "True",
  "False", "None", 0
};
static const char *const HL_PY_TYPE[] = {
  "self", "print", "len", "range", "int", "str", "list", "dict", "set",
  "tuple", "bytes", "float", "bool", "open", "isinstance", "enumerate",
  "super", 0
};

static int hl_same(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

VI_FN int hl_lang_for(const char *path) {
  const char *dot = 0, *p;
  char e[6];
  int i;
  for (p = path; *p; p++) {
    if (*p == '.') dot = p + 1;
    if (*p == '/') dot = 0;
  }
  if (!dot) return LANG_PLAIN;
  for (i = 0; i < 5 && dot[i]; i++)
    e[i] = (char)(dot[i] >= 'A' && dot[i] <= 'Z' ? dot[i] + 32 : dot[i]);
  if (dot[i]) return LANG_PLAIN;
  e[i] = 0;
#define HL_IS(x) hl_same(e, x)
  if (HL_IS("c") || HL_IS("h") || HL_IS("cpp") || HL_IS("hpp") || HL_IS("cc") ||
      HL_IS("ino") || HL_IS("ld") || HL_IS("def")) return LANG_C;
  if (HL_IS("py")) return LANG_PY;
  if (HL_IS("md") || HL_IS("mkd")) return LANG_MD;
#undef HL_IS
  return LANG_PLAIN;
}

/* The keyword lists a language completes from too (vimcore.h). */
VI_FN const char *const *hl_words(int lang, int which) {
  if (lang == LANG_C) return which ? HL_C_TYPE : HL_C_KW;
  if (lang == LANG_PY) return which ? HL_PY_TYPE : HL_PY_KW;
  return 0;
}

VI_FN int hl_wordch(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static int hl_in_list(const char *const *list, const char *s, int n) {
  int i, j;
  if (!list) return 0;
  for (i = 0; list[i]; i++) {
    for (j = 0; j < n && list[i][j] == s[j]; j++) { }
    if (j == n && list[i][n] == 0) return 1;
  }
  return 0;
}

static void hl_fill(uint8_t *col, int a, int b, int c) {
  for (; a < b; a++) col[a] = (uint8_t)c;
}

/* A number from s[i]: digits, a hex prefix, a point, a suffix. Its end. */
static int hl_number(const char *s, int n, int i) {
  i++;
  while (i < n && (hl_wordch(s[i]) || s[i] == '.')) i++;
  return i;
}

/* A quoted string from the quote at s[i], with backslash escapes. Its end
 * (past the closing quote, or n). */
static int hl_quoted(const char *s, int n, int i) {
  char q = s[i++];
  while (i < n) {
    if (s[i] == '\\') { i += 2; continue; }
    if (s[i++] == q) break;
  }
  return i > n ? n : i;
}

static int hl_c(int st, const char *s, int n, uint8_t *col) {
  int i = 0, k, first = 1;
  while (i < n) {
    if (st == HS_CCOM) {
      for (k = i; k + 1 < n && !(s[k] == '*' && s[k + 1] == '/'); k++) { }
      if (k + 1 < n) { hl_fill(col, i, k + 2, HL_COM); i = k + 2; st = HS_NONE; }
      else { hl_fill(col, i, n, HL_COM); i = n; }
      continue;
    }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') { hl_fill(col, i, n, HL_COM); break; }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
      col[i] = col[i + 1] = HL_COM;
      i += 2;
      st = HS_CCOM;
      continue;
    }
    if (first && s[i] == '#') {                /* a directive and its word */
      k = i + 1;
      while (k < n && s[k] == ' ') k++;
      while (k < n && hl_wordch(s[k])) k++;
      hl_fill(col, i, k, HL_PRE);
      i = k;
      while (i < n && s[i] == ' ') i++;
      if (i < n && s[i] == '<') {             /* #include <x.h> */
        for (k = i; k < n && s[k] != '>'; k++) { }
        if (k < n) k++;
        hl_fill(col, i, k, HL_STR);
        i = k;
      }
      first = 0;
      continue;
    }
    if (s[i] != ' ' && s[i] != '\t') first = 0;
    if (s[i] == '"' || s[i] == '\'') {
      k = hl_quoted(s, n, i);
      hl_fill(col, i, k, HL_STR);
      i = k;
      continue;
    }
    if (s[i] >= '0' && s[i] <= '9') {
      k = hl_number(s, n, i);
      hl_fill(col, i, k, HL_NUM);
      i = k;
      continue;
    }
    if (hl_wordch(s[i])) {
      for (k = i; k < n && hl_wordch(s[k]); k++) { }
      hl_fill(col, i, k, hl_in_list(HL_C_KW, s + i, k - i) ? HL_KW :
                         hl_in_list(HL_C_TYPE, s + i, k - i) ? HL_TYPE : HL_N);
      i = k;
      continue;
    }
    col[i++] = HL_N;
  }
  return st;
}

static int hl_py(int st, const char *s, int n, uint8_t *col) {
  int i = 0, k;
  while (i < n) {
    if (st == HS_PY3D || st == HS_PY3S) {
      char q = st == HS_PY3D ? '"' : '\'';
      for (k = i; k + 2 < n && !(s[k] == q && s[k + 1] == q && s[k + 2] == q); k++)
        if (s[k] == '\\') k++;
      if (k + 2 < n) { hl_fill(col, i, k + 3, HL_STR); i = k + 3; st = HS_NONE; }
      else { hl_fill(col, i, n, HL_STR); i = n; }
      continue;
    }
    if (s[i] == '#') { hl_fill(col, i, n, HL_COM); break; }
    if ((s[i] == '"' || s[i] == '\'') && i + 2 < n && s[i + 1] == s[i] && s[i + 2] == s[i]) {
      st = s[i] == '"' ? HS_PY3D : HS_PY3S;
      hl_fill(col, i, i + 3, HL_STR);
      i += 3;
      continue;
    }
    if (s[i] == '"' || s[i] == '\'') {
      k = hl_quoted(s, n, i);
      hl_fill(col, i, k, HL_STR);
      i = k;
      continue;
    }
    if (s[i] == '@' && (i == 0 || s[i - 1] == ' ')) {
      for (k = i + 1; k < n && (hl_wordch(s[k]) || s[k] == '.'); k++) { }
      hl_fill(col, i, k, HL_PRE);
      i = k;
      continue;
    }
    if (s[i] >= '0' && s[i] <= '9') {
      k = hl_number(s, n, i);
      hl_fill(col, i, k, HL_NUM);
      i = k;
      continue;
    }
    if (hl_wordch(s[i])) {
      for (k = i; k < n && hl_wordch(s[k]); k++) { }
      hl_fill(col, i, k, hl_in_list(HL_PY_KW, s + i, k - i) ? HL_KW :
                         hl_in_list(HL_PY_TYPE, s + i, k - i) ? HL_TYPE : HL_N);
      i = k;
      continue;
    }
    col[i++] = HL_N;
  }
  return st;
}

static int hl_md(int st, const char *s, int n, uint8_t *col) {
  int i = 0, k;
  while (i < n && s[i] == ' ') col[i++] = HL_N;
  if (n - i >= 3 && s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`') {
    hl_fill(col, 0, n, HL_CODE);
    return st == HS_MDFENCE ? HS_NONE : HS_MDFENCE;
  }
  if (st == HS_MDFENCE) { hl_fill(col, 0, n, HL_CODE); return st; }
  if (i < n && s[i] == '#') { hl_fill(col, 0, n, HL_HEAD); return st; }
  if (i < n && s[i] == '>') { hl_fill(col, 0, n, HL_COM); return st; }
  if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
    col[i] = HL_KW;
    i++;
  } else if (i < n && s[i] >= '0' && s[i] <= '9') {
    for (k = i; k < n && s[k] >= '0' && s[k] <= '9'; k++) { }
    if (k < n && (s[k] == '.' || s[k] == ')')) { hl_fill(col, i, k + 1, HL_KW); i = k + 1; }
  }
  while (i < n) {
    if (s[i] == '`') {
      for (k = i + 1; k < n && s[k] != '`'; k++) { }
      if (k < n) k++;
      hl_fill(col, i, k, HL_CODE);
      i = k;
      continue;
    }
    if (s[i] == '*' || (s[i] == '_' && (i == 0 || !hl_wordch(s[i - 1])))) {
      char m = s[i];
      int two = i + 1 < n && s[i + 1] == m;
      for (k = i + 1 + two; k < n && s[k] != m; k++) { }
      if (k < n && k > i + 1 + two) {
        k += 1 + two;
        if (k > n) k = n;
        hl_fill(col, i, k, HL_EMPH);
        i = k;
        continue;
      }
    }
    if (s[i] == '[') {
      for (k = i; k < n && s[k] != ']'; k++) { }
      if (k + 1 < n && s[k + 1] == '(') {
        int e;
        for (e = k + 1; e < n && s[e] != ')'; e++) { }
        if (e < n) e++;
        hl_fill(col, i, k + 1, HL_KW);
        hl_fill(col, k + 1, e, HL_STR);
        i = e;
        continue;
      }
    }
    col[i++] = HL_N;
  }
  return st;
}

/* Colour s[0..n) into col[0..n), starting in state st; the state at its end. */
VI_FN int hl_line(int lang, int st, const char *s, int n, uint8_t *col) {
  if (lang == LANG_C) return hl_c(st, s, n, col);
  if (lang == LANG_PY) return hl_py(st, s, n, col);
  if (lang == LANG_MD) return hl_md(st, s, n, col);
  hl_fill(col, 0, n, HL_N);
  return HS_NONE;
}

#endif /* CARDOS_VIMHL_H */
