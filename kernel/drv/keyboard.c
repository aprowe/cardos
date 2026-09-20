/* Cardputer keyboard matrix.
 *
 * The scan and the coordinate mapping below are transcribed from M5Stack's own
 * IOMatrix reader, which is the only authoritative description of how this
 * matrix is wired. Getting it wrong yields a keyboard that reports plausible
 * but wrong keys, which is far harder to debug than one that reports nothing.
 *
 *   address lines (into the 74HC138): GPIO 8 (bit 0), 9 (bit 1), 11 (bit 2)
 *   row inputs, pulled up, active low: GPIO 13, 15, 3, 4, 5, 6, 7
 *
 * For each of the 8 column selects, the seven rows are read. A set bit j maps
 * to logical x = 2j for column selects 4..7 and x = 2j+1 for 0..3; y counts
 * down from 3. That asymmetry is the physical interleave of the key columns,
 * not a mistake.
 */

#include "kernel/drv/keyboard.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us */

static const int ADDR_PINS[3] = { 8, 9, 11 };
static const int ROW_PINS[7]  = { 13, 15, 3, 4, 5, 6, 7 };

/* The 4x14 layout printed on the keys. Row 0 is the number row. */
static const char KEYMAP[4][14] = {
  { '`','1','2','3','4','5','6','7','8','9','0','-','=', (char)KEY_BACKSPACE },
  { (char)KEY_TAB,'q','w','e','r','t','y','u','i','o','p','[',']','\\' },
  { 0 /*Fn*/, 0 /*Shift*/,'a','s','d','f','g','h','j','k','l',';','\'', (char)KEY_ENTER },
  { 0 /*Ctrl*/, 0 /*Opt*/, 0 /*Alt*/,'z','x','c','v','b','n','m',',','.','/',' ' },
};

static const char KEYMAP_SHIFT[4][14] = {
  { '~','!','@','#','$','%','^','&','*','(',')','_','+', (char)KEY_BACKSPACE },
  { (char)KEY_TAB,'Q','W','E','R','T','Y','U','I','O','P','{','}','|' },
  { 0, 0,'A','S','D','F','G','H','J','K','L',':','"', (char)KEY_ENTER },
  { 0, 0, 0,'Z','X','C','V','B','N','M','<','>','?',' ' },
};

/* Modifier positions in the map above. */
#define IS_FN(x, y)    ((y) == 2 && (x) == 0)
#define IS_SHIFT(x, y) ((y) == 2 && (x) == 1)
#define IS_CTRL(x, y)  ((y) == 3 && (x) == 0)
#define IS_ALT(x, y)   ((y) == 3 && (x) == 2)
#define IS_OPT(x, y)   ((y) == 3 && (x) == 1)

static uint8_t s_down[4][14];      /* previous scan, for edge detection */
static int s_shift, s_ctrl, s_fn, s_opt;

static void set_address(int value) {
  gpio_set_level(ADDR_PINS[0], (value >> 0) & 1);
  gpio_set_level(ADDR_PINS[1], (value >> 1) & 1);
  gpio_set_level(ADDR_PINS[2], (value >> 2) & 1);
}

int keyboard_init(void) {
  int i;
  gpio_config_t out = {
    .pin_bit_mask = 0,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config_t in = {
    .pin_bit_mask = 0,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,      /* switches pull a row to ground */
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };

  for (i = 0; i < 3; i++) out.pin_bit_mask |= 1ULL << ADDR_PINS[i];
  for (i = 0; i < 7; i++) in.pin_bit_mask  |= 1ULL << ROW_PINS[i];

  if (gpio_config(&out) != ESP_OK) return -1;
  if (gpio_config(&in) != ESP_OK) return -1;

  set_address(0);
  for (i = 0; i < 4 * 14; i++) ((uint8_t *)s_down)[i] = 0;
  return 0;
}

/* Scan the whole matrix into `now`, and update the modifier state. */
static void scan(uint8_t now[4][14]) {
  int col, j, x, y;

  for (y = 0; y < 4; y++)
    for (x = 0; x < 14; x++) now[y][x] = 0;

  s_shift = s_ctrl = s_fn = s_opt = 0;

  for (col = 0; col < 8; col++) {
    set_address(col);
    /* The 74HC138 and the row pull-ups need a moment to settle before the
     * read; without this the first column bleeds into the next. */
    esp_rom_delay_us(10);

    for (j = 0; j < 7; j++) {
      if (gpio_get_level(ROW_PINS[j]) != 0) continue;   /* active low */

      x = (col > 3) ? (2 * j) : (2 * j + 1);
      y = 3 - ((col > 3) ? (col - 4) : col);
      if (x < 0 || x >= 14 || y < 0 || y >= 4) continue;

      now[y][x] = 1;
      if (IS_SHIFT(x, y)) s_shift = 1;
      if (IS_CTRL(x, y))  s_ctrl = 1;
      if (IS_FN(x, y))    s_fn = 1;
      if (IS_OPT(x, y))   s_opt = 1;
    }
  }
}

/* Is this key held down *right now*?
 *
 * Not an edge like keyboard_poll -- a level, sampled for a while. Safe mode
 * asks this at boot, where there is no key event to catch: the key was already
 * down before the machine started, so the rising edge happened while the
 * matrix was unpowered and nobody was looking.
 *
 * `settle_ms` is scanned rather than slept through, because a key pressed as
 * the board comes up may not read as down on the very first scan. Only the
 * unshifted map is consulted; a modifier cannot be the safe-mode key. */
int keyboard_held(uint8_t key, int settle_ms) {
  uint8_t now[4][14];
  int waited = 0;

  for (;;) {
    int x, y;
    scan(now);
    for (y = 0; y < 4; y++) {
      for (x = 0; x < 14; x++) {
        char c = KEYMAP[y][x];
        if (!now[y][x]) continue;
        if (c == '`') c = (char)KEY_ESC;
        if ((uint8_t)c == key) return 1;
      }
    }
    if (waited >= settle_ms) return 0;
    /* A busy wait rather than a task delay: this runs before the shell exists
     * and there is nothing else to yield to, and 20 ms of spin at boot is
     * invisible next to the display bring-up above it. */
    esp_rom_delay_us(20000);
    waited += 20;
  }
}

/* Is any key down at this instant?
 *
 * A peek, not a read: it consumes nothing, so the keypress it saw is still
 * there for keyboard_poll to deliver properly afterwards. It exists for code
 * that is blocked in a long operation and wants to know it should stop --
 * the screen viewer sits inside an HTTP stream that never ends, and this is
 * how escape reaches it. */
int keyboard_any_down(void) {
  uint8_t now[4][14];
  int x, y;

  scan(now);
  for (y = 0; y < 4; y++)
    for (x = 0; x < 14; x++)
      if (now[y][x] && !IS_SHIFT(x, y) && !IS_CTRL(x, y) && !IS_FN(x, y) &&
          !IS_OPT(x, y) && !IS_ALT(x, y))
        return 1;
  return 0;
}

uint8_t keyboard_poll(void) {
  uint8_t now[4][14];
  uint8_t out = 0;
  int x, y;

  scan(now);

  for (y = 0; y < 4 && !out; y++) {
    for (x = 0; x < 14; x++) {
      if (!now[y][x] || s_down[y][x]) continue;    /* only rising edges */
      if (IS_SHIFT(x, y) || IS_CTRL(x, y) || IS_FN(x, y) || IS_OPT(x, y) ||
          IS_ALT(x, y)) continue;                  /* modifiers are not keys */

      {
        char c = s_shift ? KEYMAP_SHIFT[y][x] : KEYMAP[y][x];

        /* The ` key is labelled ESC on the case and is the universal escape,
         * so it never reaches anyone as a backtick. */
        if (KEYMAP[y][x] == '`') c = (char)KEY_ESC;

        /* Ctrl plus a letter gives the usual control character, so desktop
         * chords work identically from this keyboard and from a serial
         * terminal -- and so ordinary letters stay free for whatever has
         * focus. */
        /* Opt first: a global shortcut outranks whatever the key would
         * otherwise mean, which is the point of having one. */
        if (s_opt) {
          char base = KEYMAP[y][x];
          if (base >= '0' && base <= '9') c = (char)KEY_OPT_DIGIT(base - '0');
          else if (base >= 'a' && base <= 'z') c = (char)KEY_OPT_LETTER(base);
          /* opt-backspace leaves the app, the same as fn-`: the two keys sit
           * on opposite corners and either hand finds one of them. */
          else if (base == (char)KEY_BACKSPACE) c = (char)KEY_QUIT;
          else c = 0;
        }
        else if (s_ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (s_ctrl && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);

        /* Fn is the window modifier: ; , . / are the arrow cluster, and a
         * letter is a chord of its own. Like opt's, those get codes above the
         * ASCII range so they survive being typed inside a text field -- fn-w
         * has to close a window while an editor is swallowing every letter.
         * See the convention in keyboard.h. */
        if (s_fn) {
          char base = KEYMAP[y][x];
          switch (base) {
          case ';': c = (char)KEY_UP;    break;
          case '.': c = (char)KEY_DOWN;  break;
          case ',': c = (char)KEY_LEFT;  break;
          case '/': c = (char)KEY_RIGHT; break;
          /* The key labelled ESC. Alone it is Escape, which an app may keep
           * for going back a level; with fn it always leaves. */
          case '`': c = (char)KEY_QUIT;  break;
          default:
            if (base >= 'a' && base <= 'z') c = (char)KEY_FN_LETTER(base);
            break;
          }
        }
        /* Taken, whether or not it meant anything: a chord with no meaning
         * is consumed, not retried every scan for as long as it is held. */
        s_down[y][x] = 1;
        if (c) { out = (uint8_t)c; break; }
      }
    }
  }

  /* Releases and modifiers are recorded now; a second key that went down in
   * this same scan is not. Copying the whole matrix here marked it as
   * already down, and it was never delivered -- with the idle poll at 25 ms
   * the second letter of a fast pair went missing. It stays a rising edge
   * for the next poll instead. */
  for (y = 0; y < 4; y++)
    for (x = 0; x < 14; x++) {
      if (!now[y][x]) s_down[y][x] = 0;
      else if (IS_SHIFT(x, y) || IS_CTRL(x, y) || IS_FN(x, y) || IS_OPT(x, y) ||
               IS_ALT(x, y)) s_down[y][x] = 1;
    }

  return out;
}

int keyboard_shift_down(void) { return s_shift; }
int keyboard_ctrl_down(void)  { return s_ctrl; }
int keyboard_fn_down(void)    { return s_fn; }
int keyboard_opt_down(void)   { return s_opt; }

uint8_t keyboard_arrow_for(uint8_t k) {
  switch (k) {
  case ';': return KEY_UP;
  case '.': return KEY_DOWN;
  case ',': return KEY_LEFT;
  case '/': return KEY_RIGHT;
  default:  return 0;
  }
}
