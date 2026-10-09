/* The answer to "delete this? y/n", the same in every app.
 *
 * CLAUDE.md's key table says it: y is yes; n, Escape and Backspace are no.
 * Before this the question was written fourteen times and answered three
 * ways -- Explorer took Enter as yes, half the apps took any other key as
 * no, the rest waited -- and only four ignored a held key's repeats, so in
 * Files or Music a held d repeated into its own question 400 ms later and
 * answered it.
 *
 * The rule now: y or Y is yes. n, N, Escape and Backspace are no. A
 * repeat is nothing (the key that asked is still down). Anything else is
 * nothing too and the question stays up -- Enter in particular, which is
 * the key a thumb is already on.
 *
 *   if (asking) {
 *     int a = confirm_key(api, k);
 *     if (a == CONFIRM_YES) delete_it();
 *     if (a != CONFIRM_WAIT) asking = 0;
 *     return 1;
 *   }
 *
 * Header-only, static helpers, the same arrangement as apps/safefile.h.
 */
#ifndef CARDOS_CONFIRM_H
#define CARDOS_CONFIRM_H

#include "kernel/app/capp.h"

#if defined(__GNUC__)
#define CONFIRM_OPT __attribute__((unused))
#else
#define CONFIRM_OPT
#endif

#define CONFIRM_WAIT 0          /* not an answer: keep asking */
#define CONFIRM_YES  1
#define CONFIRM_NO   2

static CONFIRM_OPT int confirm_key(const CardApi *api, uint8_t k) {
  if (api->key_repeat()) return CONFIRM_WAIT;
  if (k == 'y' || k == 'Y') return CONFIRM_YES;
  if (k == 'n' || k == 'N' || k == CAPP_KEY_ESC || k == CAPP_KEY_BACK) return CONFIRM_NO;
  return CONFIRM_WAIT;
}

#endif /* CARDOS_CONFIRM_H */
