/* The "it has not crashed" indicator. Device-only.
 *
 * The shell is one cooperative loop. A blocking call -- an HTTPS request, an
 * NTP round trip, a token exchange -- happens inside it, so nothing repaints
 * and nothing responds until it returns. For twenty seconds the machine is
 * indistinguishable from a hung one, and the honest fix is not to make the
 * call faster but to say that it is happening.
 *
 * A separate task does the drawing, because the thread that would otherwise
 * animate anything is precisely the one that is blocked. It was first made
 * safe by an assumption: between busy_begin and busy_end the calling thread
 * is inside a network call and is not drawing, so there is exactly one writer
 * to the panel. `update os` broke it -- it logs progress to the console from
 * inside the call -- and two tasks in the SPI panel driver at once is an
 * assert and a reboot. So display_blit now takes a lock, and the panel is
 * safe whoever draws; what a collision costs today is a badge frame drawn
 * over a console line, which the repaint at busy_end cleans up. The mutex
 * here still covers the frame boundary, so busy_end cannot return while a
 * frame is half sent.
 *
 * THE RULE THIS BENDS. kernel/sys/bg.c's jobs must never touch the display,
 * and that rule is right: the background task runs while the shell is drawing.
 * This is a different case -- a task that only ever draws while the shell
 * cannot -- which is why it is its own module and not another bg job.
 */
#ifndef CARDOS_BUSY_H
#define CARDOS_BUSY_H

void busy_init(void);

/* Show the indicator. `what` is a few words -- "fetching", "signing in" --
 * shown beside the spinner; NULL for the spinner alone. Nesting is counted,
 * so an outer call that makes three requests shows one continuous indicator
 * rather than three that flicker. */
void busy_begin(const char *what);
void busy_end(void);

/* How the shell is told to paint over the hole the indicator leaves. There is
 * no framebuffer to restore from, so the only way back is to redraw. */
void busy_on_done(void (*repaint)(void));

/* Is it showing? For anything that draws and would otherwise fight it. */
int  busy_active(void);

#endif /* CARDOS_BUSY_H */
