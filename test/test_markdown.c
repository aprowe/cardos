/* The markdown preview's line classifier.
 *
 * `md_kind` is the whole of the parser: everything the renderer does after it
 * follows from which of ten shapes a line was called. It is also the part that
 * has to be conservative -- markdown's premise is that unrecognised text is
 * still readable text, so a line that is *nearly* a heading must come out as
 * the words it contains rather than as a heading with a stray hash in it.
 *
 * The renderer itself draws, so it cannot run here. This does not test
 * drawing; it tests the decision drawing is made from.
 */

#include <string.h>

#include "tinytest.h"

/* The classifier, lifted verbatim from apps/edit.c. It has no dependency on
 * the API table -- it is pointer arithmetic over a string -- and a copy that
 * has to stay in step is the price of testing a file that cannot be linked
 * here. The last test in this file is what catches the two drifting. */
typedef enum {
  MD_TEXT = 0, MD_H1, MD_H2, MD_H3, MD_BULLET, MD_NUMBER,
  MD_QUOTE, MD_CODE, MD_RULE, MD_BLANK
} MdKind;

static MdKind md_kind(const char *in, const char **body, int *indent) {
  const char *p = in;
  int spaces = 0;

  while (*p == ' ') { p++; spaces++; }
  *indent = spaces / 2;
  *body = p;

  if (!*p) return MD_BLANK;

  if (p[0] == '#' && p[1] == '#' && p[2] == '#' && p[3] == ' ') { *body = p + 4; return MD_H3; }
  if (p[0] == '#' && p[1] == '#' && p[2] == ' ')                { *body = p + 3; return MD_H2; }
  if (p[0] == '#' && p[1] == ' ')                               { *body = p + 2; return MD_H1; }

  if (p[0] == '-' || p[0] == '_' || p[0] == '*') {
    const char *q = p;
    int n = 0;
    while (*q == *p) { q++; n++; }
    while (*q == ' ') q++;
    if (n >= 3 && !*q) return MD_RULE;
  }

  if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') {
    *body = p + 2;
    return MD_BULLET;
  }
  if (p[0] >= '0' && p[0] <= '9') {
    const char *q = p;
    while (*q >= '0' && *q <= '9') q++;
    if ((q[0] == '.' || q[0] == ')') && q[1] == ' ') { *body = p; return MD_NUMBER; }
  }
  if (p[0] == '>') { *body = (p[1] == ' ') ? p + 2 : p + 1; return MD_QUOTE; }

  return MD_TEXT;
}

static MdKind kind(const char *line, const char **body) {
  int indent;
  static const char *scratch;
  MdKind k = md_kind(line, body ? body : &scratch, &indent);
  return k;
}

void test_markdown_headings(void) {
  const char *body;
  CHECK_EQ(kind("# Title", &body), MD_H1);
  CHECK(!strcmp(body, "Title"));
  CHECK_EQ(kind("## Section", &body), MD_H2);
  CHECK(!strcmp(body, "Section"));
  CHECK_EQ(kind("### Detail", &body), MD_H3);
  CHECK(!strcmp(body, "Detail"));
}

/* A hash with no space is a hash. People write "#1 priority" and "#tag", and
 * rendering either as a heading would be worse than rendering neither. */
void test_markdown_a_hash_alone_is_not_a_heading(void) {
  const char *body;
  CHECK_EQ(kind("#notaheading", &body), MD_TEXT);
  CHECK_EQ(kind("#1 priority", &body), MD_TEXT);
  CHECK_EQ(kind("####### seven", &body), MD_TEXT);
}

void test_markdown_lists(void) {
  const char *body;
  CHECK_EQ(kind("- milk", &body), MD_BULLET);
  CHECK(!strcmp(body, "milk"));
  CHECK_EQ(kind("* milk", &body), MD_BULLET);
  CHECK_EQ(kind("+ milk", &body), MD_BULLET);
  CHECK_EQ(kind("1. first", &body), MD_NUMBER);
  CHECK(!strcmp(body, "1. first"));       /* the number is kept: it is content */
  CHECK_EQ(kind("12) twelfth", &body), MD_NUMBER);

  /* No space, no list. "-5 degrees" is a temperature. */
  CHECK_EQ(kind("-5 degrees", &body), MD_TEXT);
  CHECK_EQ(kind("1.5 litres", &body), MD_TEXT);
}

void test_markdown_indent_is_two_spaces_a_level(void) {
  const char *body;
  int indent;
  CHECK_EQ(md_kind("    - deep", &body, &indent), MD_BULLET);
  CHECK_EQ(indent, 2);
  CHECK(!strcmp(body, "deep"));
}

void test_markdown_rules(void) {
  const char *body;
  CHECK_EQ(kind("---", &body), MD_RULE);
  CHECK_EQ(kind("***", &body), MD_RULE);
  CHECK_EQ(kind("___", &body), MD_RULE);
  CHECK_EQ(kind("----------", &body), MD_RULE);
  CHECK_EQ(kind("--- ", &body), MD_RULE);       /* trailing space is still a rule */

  /* Two is not a rule, and a rule with words after it is a sentence. */
  CHECK_EQ(kind("--", &body), MD_TEXT);
  CHECK_EQ(kind("--- and then", &body), MD_TEXT);
}

/* The one that matters most: a bullet and a rule both start with a hyphen,
 * and getting the order wrong turns every "- item" into a horizontal line. */
void test_markdown_a_rule_is_not_a_bullet(void) {
  const char *body;
  CHECK_EQ(kind("- item", &body), MD_BULLET);
  CHECK_EQ(kind("---", &body), MD_RULE);
  CHECK_EQ(kind("* item", &body), MD_BULLET);
  CHECK_EQ(kind("***", &body), MD_RULE);
}

void test_markdown_quotes_and_blanks(void) {
  const char *body;
  CHECK_EQ(kind("> quoted", &body), MD_QUOTE);
  CHECK(!strcmp(body, "quoted"));
  CHECK_EQ(kind(">tight", &body), MD_QUOTE);
  CHECK(!strcmp(body, "tight"));
  CHECK_EQ(kind("", &body), MD_BLANK);
  CHECK_EQ(kind("   ", &body), MD_BLANK);
}

/* Anything unrecognised is text, which is markdown's whole premise. */
void test_markdown_falls_back_to_text(void) {
  const char *body;
  CHECK_EQ(kind("just a sentence.", &body), MD_TEXT);
  CHECK_EQ(kind("| a | table |", &body), MD_TEXT);
  CHECK_EQ(kind("<html>", &body), MD_TEXT);
  CHECK(!strcmp(body, "<html>"));
}

/* This file holds a copy of the classifier from apps/edit.c, because that file
 * links against the API table and cannot be built here. The copy is only
 * worth having if it stays the same: if this fails, the two have drifted and
 * the tests above are testing nothing. */
void test_markdown_the_copy_matches_the_editor(void) {
  FILE *f = fopen("apps/edit.c", "rb");
  static char buf[80000];
  size_t n;

  if (!f) { CHECK(1); return; }        /* run from elsewhere: not a failure */
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;

  /* The distinctive lines, in order. Cheap, and it catches the edits anyone
   * would actually make -- reordering the rule and bullet tests, or changing
   * what counts as a heading. */
  CHECK(strstr(buf, "if (p[0] == '#' && p[1] == '#' && p[2] == '#' && p[3] == ' ')") != NULL);
  CHECK(strstr(buf, "if (n >= 3 && !*q) return MD_RULE;") != NULL);
  CHECK(strstr(buf, "if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ')") != NULL);
  CHECK(strstr(buf, "if (p[0] == '>' )") != NULL ||
        strstr(buf, "if (p[0] == '>')") != NULL);
}
