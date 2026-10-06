/* Small sounds the OS makes: a tick as the launcher moves, a rise as an app
 * opens and a fall as it closes, a buzz for "that did not work", a chime
 * for "done", a click per key.
 *
 * Synthesised, not files: each is a few notes with an envelope, made in
 * memory on a task of its own and written to the speaker, so asking costs
 * the caller nothing. A blip never interrupts anything: while music or a
 * memo plays, or the mic records (G43 is both the mic's clock and the
 * speaker's LRCLK), it is simply not made. Settings > Sound turns them off,
 * and the key clicks separately. */
#ifndef CARDOS_BLIP_H
#define CARDOS_BLIP_H

typedef enum {
  BLIP_KEY = 0,     /* a key pressed: the quietest */
  BLIP_MOVE,        /* the selection moved */
  BLIP_SOFT,        /* something small opened: search, a menu */
  BLIP_OPEN,        /* an app or folder opened: rising */
  BLIP_BACK,        /* closed, went back: falling */
  BLIP_ERROR,       /* that did not work */
  BLIP_DONE,        /* finished: saved, sent, printed */
  BLIP_NOTIFY,      /* look at me */
  BLIP_BOOT,        /* CardOS is up */
  BLIP_COUNT
} Blip;

void blip_init(void);
void blip(Blip which);

/* Settings > Sound: UI sounds, and key clicks (which also need UI sounds on). */
int  blip_ui_on(void);
void blip_set_ui(int on);
int  blip_keys_on(void);
void blip_set_keys(int on);

#endif /* CARDOS_BLIP_H */
