/* Mouse input. See mouse.h. */

#include "kernel/input/mouse.h"

static int16_t g_w = 240, g_h = 135;
static int16_t g_x, g_y;
static uint8_t g_buttons;      /* current level */
static uint8_t g_pressed;      /* edges not yet consumed */
static uint8_t g_released;
static int     g_wheel;
static int     g_moved;

int mouse_decode_boot(const uint8_t *report, size_t len, MouseReport *out) {
  if (!report || !out) return -1;

  /* Some devices prefix a one-byte report ID. The boot layout proper is 3 or
   * 4 bytes, so anything longer than 4 is assumed to carry one. */
  if (len > 4) { report++; len--; }
  if (len < 3) return -1;

  out->buttons = report[0];
  out->dx = (int8_t)report[1];        /* signed: a cast, not a copy */
  out->dy = (int8_t)report[2];
  out->wheel = (len >= 4) ? (int8_t)report[3] : 0;
  return 0;
}

void mouse_init(int16_t w, int16_t h) {
  g_w = w;
  g_h = h;
  g_x = (int16_t)(w / 2);             /* start where the user can see it */
  g_y = (int16_t)(h / 2);
  g_buttons = g_pressed = g_released = 0;
  g_wheel = 0;
  g_moved = 0;
}

void mouse_apply(const MouseReport *r) {
  int32_t nx, ny;
  uint8_t changed;

  if (!r) return;

  /* Clamp the position, not the delta. Clamping the delta would leave the
   * cursor owing movement after it hit an edge, so it would sit there for a
   * few reports before setting off again. */
  nx = (int32_t)g_x + r->dx;
  ny = (int32_t)g_y + r->dy;
  if (nx < 0) nx = 0;
  if (ny < 0) ny = 0;
  if (nx > g_w - 1) nx = g_w - 1;
  if (ny > g_h - 1) ny = g_h - 1;

  if (nx != g_x || ny != g_y) g_moved = 1;
  g_x = (int16_t)nx;
  g_y = (int16_t)ny;

  changed = (uint8_t)(r->buttons ^ g_buttons);
  g_pressed  |= (uint8_t)(changed & r->buttons);
  g_released |= (uint8_t)(changed & g_buttons);
  g_buttons = r->buttons;

  g_wheel += r->wheel;
}

int16_t mouse_x(void) { return g_x; }
int16_t mouse_y(void) { return g_y; }

int mouse_down(uint8_t button) { return (g_buttons & button) ? 1 : 0; }

int mouse_pressed(uint8_t button) {
  int hit = (g_pressed & button) ? 1 : 0;
  g_pressed = (uint8_t)(g_pressed & ~button);
  return hit;
}

int mouse_released(uint8_t button) {
  int hit = (g_released & button) ? 1 : 0;
  g_released = (uint8_t)(g_released & ~button);
  return hit;
}

int mouse_take_wheel(void) {
  int w = g_wheel;
  g_wheel = 0;
  return w;
}

int mouse_take_moved(void) {
  int m = g_moved;
  g_moved = 0;
  return m;
}
