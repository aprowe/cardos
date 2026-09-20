/* The file picker: a panel the OS draws over an app while the app waits.
 *
 * An app asks (api->pick), returns to its loop, and polls (api->pick_poll)
 * from tick until an answer comes -- the shape http_start/http_poll has,
 * for the same reason: the shell is one cooperative loop, and a picker that
 * blocked inside a key handler would be a picker with no keys. While it is
 * up, the shells route every key, click and paint here and the app gets
 * none of them; when it closes, the shell repaints what it covered.
 *
 * The behaviour is kernel/ui/pickmodel.c, which is portable and tested.
 * This file paints it and gives it the card. One picker at a time. */
#ifndef CARDOS_PICKER_H
#define CARDOS_PICKER_H

#include <stdint.h>
#include <stddef.h>

#include "kernel/ui/pickmodel.h"

#define PICKER_PENDING (-1000)

/* Put it up. Strings are copied. 0, or -1 if one is already open. */
int  picker_open(int mode, const char *title, const char *dir,
                 const char *filter, const char *name);

int  picker_active(void);

/* PICKER_PENDING while open; then 1 with the chosen path in `out`, or 0 for
 * cancelled. The answer is handed over once: the next call after that is
 * PICKER_PENDING again, meaning "nothing is being asked". */
int  picker_poll(char *out, size_t n);

/* What the shells call while it is active. `key` and `click` return 1 when
 * the picker just closed, so the shell knows to repaint itself. */
void picker_paint(void);         /* only when something changed */
void picker_paint_now(void);     /* everything, unconditionally */
int  picker_key(uint8_t k, uint32_t now_ms);
/* Is a name being typed? Otherwise ; , . / are the arrows, unmodified, as
 * they are everywhere a list has the keys. */
int  picker_wants_text(void);
int  picker_click(int16_t x, int16_t y, int button);
void picker_wheel(int dy);

/* Drop it as if cancelled -- a shell leaving the app that asked. */
void picker_close(void);

/* The help panel's lines for it. */
const char *picker_help(void);

#endif /* CARDOS_PICKER_H */
