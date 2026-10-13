/* Chat's dog emojis (apps/chat/emoji.h): a message that is exactly
 * ":dog-mood:" is a picture, anything else stays text, and a row of the
 * picture comes out as the generator drew it -- the outline round the dog,
 * Chat's background where there is nothing. */
#include "tinytest.h"

#include <string.h>

#include "kernel/app/capp.h"
#include "apps/chat/emoji.h"

#define BG 0x1234

/* The palette is in the panel's order, as an app's own colours are. */
static uint16_t rgb(int r, int g, int b) { return CAPP_RGB(r, g, b); }

void test_chat_emoji_parse(void) {
  CHECK_EQ(emo_parse(":arty-happy:"), 0);
  CHECK_EQ(emo_parse(":arty-love:"), 1);
  CHECK_EQ(emo_parse(":marcy-happy:"), EMO_MOODS);
  CHECK_EQ(emo_parse(":marcy-huh:"), EMO_DOGS * EMO_MOODS - 1);
  CHECK_EQ(emo_parse("  :arty-sad: "), 2);          /* spaces round it are nothing */
  CHECK_EQ(emo_parse(":arty-happy: hi"), -1);       /* in a sentence: text */
  CHECK_EQ(emo_parse("arty-happy"), -1);
  CHECK_EQ(emo_parse(":arty-happy"), -1);
  CHECK_EQ(emo_parse(":rex-happy:"), -1);
  CHECK_EQ(emo_parse(":arty-dance:"), -1);
  CHECK_EQ(emo_parse(":arty-happyx:"), -1);
  CHECK_EQ(emo_parse(":arty-:"), -1);
  CHECK_EQ(emo_parse(""), -1);
  CHECK_EQ(emo_parse("::"), -1);
}

void test_chat_emoji_code_round_trip(void) {
  char b[24];
  int i;
  for (i = 0; i < EMO_DOGS * EMO_MOODS; i++) {
    CHECK(emo_text(b, sizeof b, i) > 0);
    CHECK_EQ(emo_parse(b), i);
  }
  CHECK(!strcmp((emo_text(b, sizeof b, 3), b), ":arty-sleepy:"));
  CHECK_EQ(emo_text(b, 8, 0), 0);                   /* does not fit: nothing */
  CHECK_EQ(emo_text(b, sizeof b, 99), 0);
}

void test_chat_emoji_rows(void) {
  uint16_t row[EMO_W * 2];
  /* Arty's left ear tip is sprite (row 1, col 2): fur, the outline left of
   * it, and nothing at all left of that. Twice the size on the device. */
  emo_row(0, 2, 2, BG, row);
  CHECK_EQ(row[0], BG);
  CHECK_EQ(row[1], BG);
  CHECK_EQ(row[2], rgb(112, 118, 138));
  CHECK_EQ(row[3], rgb(112, 118, 138));
  CHECK_EQ(row[4], rgb(34, 32, 38));
  CHECK_EQ(row[5], rgb(34, 32, 38));
  /* The top row is above the dog: empty, except where a mark is. */
  emo_row(0, 0, 2, BG, row);
  CHECK_EQ(row[0], BG);
  CHECK_EQ(row[2 * EMO_W - 1], BG);                 /* happy has no mark */
  emo_row(1, 0, 2, BG, row);                         /* love: a heart, top right */
  CHECK_EQ(row[2 * 18], rgb(232, 54, 80));
  /* At its own size a row is EMO_W pixels, the same pixels. */
  {
    uint16_t one[EMO_W];
    emo_row(0, 1, 1, BG, one);
    CHECK_EQ(one[2], rgb(34, 32, 38));
    CHECK_EQ(one[1], rgb(112, 118, 138));
  }
}
