/* One value per line: what a person types into /config/wifi.txt has to come
 * back as the same values, and a line that is missing must not shift the
 * ones after it. */
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/conf.h"

#define W 40

void test_conf_splits_lines_in_order(void) {
  char lines[3][W];
  CHECK_EQ(conf_split("Ocean Beach Coffee and Plants\nButton297\n", &lines[0][0], 3, W), 2);
  CHECK(!strcmp(lines[0], "Ocean Beach Coffee and Plants"));
  CHECK(!strcmp(lines[1], "Button297"));
  CHECK(!strcmp(lines[2], ""));
}

void test_conf_forgives_cr_and_trailing_blanks(void) {
  char lines[2][W];
  CHECK_EQ(conf_split("net  \r\npass\t\r\n", &lines[0][0], 2, W), 2);
  CHECK(!strcmp(lines[0], "net"));
  CHECK(!strcmp(lines[1], "pass"));
}

void test_conf_keeps_a_blank_line_in_place(void) {
  char lines[3][W];
  /* an open network: the password line is empty, the third value stays third */
  CHECK_EQ(conf_split("open-net\n\nthird\n", &lines[0][0], 3, W), 3);
  CHECK(!strcmp(lines[0], "open-net"));
  CHECK(!strcmp(lines[1], ""));
  CHECK(!strcmp(lines[2], "third"));
}

void test_conf_handles_no_final_newline_and_long_lines(void) {
  char lines[2][8];
  CHECK_EQ(conf_split("abcdefghijkl\nxy", &lines[0][0], 2, 8), 2);
  CHECK(!strcmp(lines[0], "abcdefg"));      /* cut to fit, NUL kept */
  CHECK(!strcmp(lines[1], "xy"));
  CHECK_EQ(conf_split("", &lines[0][0], 2, 8), 0);
  CHECK_EQ(conf_split(NULL, &lines[0][0], 2, 8), 0);
}

void test_conf_joins_and_round_trips(void) {
  const char *vals[] = { "id", "", "tok" };
  char text[64], lines[3][W];
  CHECK_EQ(conf_join(vals, 3, text, sizeof text), 8);
  CHECK(!strcmp(text, "id\n\ntok\n"));
  CHECK_EQ(conf_split(text, &lines[0][0], 3, W), 3);
  CHECK(!strcmp(lines[1], ""));
  CHECK(!strcmp(lines[2], "tok"));
  CHECK_EQ(conf_join(vals, 3, text, 6), -1);   /* too small: refused, not truncated */
}
