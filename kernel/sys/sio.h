/* Standard input and output for command-line tools. Device-only.
 *
 * Named sio_ rather than stdio_ because picolibc owns that name, and this is
 * the third collision of its kind in this tree.
 *
 * A tool writes with sio_write and reads with sio_read_line, and does not know
 * or care where either goes. The shell decides: the console, a file, or a
 * buffer that becomes the next tool's input.
 *
 * Pipes are buffered rather than concurrent. CardOS runs one cooperative loop
 * with no context switch, so "both programs run at once" is not available --
 * the left-hand side runs to completion into a buffer and the right-hand side
 * then reads it. The only thing that changes is an infinite producer, and
 * there are none here. Saying so plainly is better than a pipe that looks
 * concurrent and deadlocks the first time someone writes one.
 */
#ifndef CARDOS_SIO_H
#define CARDOS_SIO_H

#include <stddef.h>

/* Everything back to the console, and stdin empty. Call between commands. */
void sio_reset(void);

/* ---- stdout ---- */

void sio_write(const char *s);
void sio_write_line(const char *s);      /* adds the newline */

/* Capture what is written, up to `cap` bytes. Returns 0 if the buffer could
 * not be allocated. Output past the cap is dropped, and sio_out_overflowed
 * says so -- a silently truncated pipe is a bug you find much later. */
int  sio_out_to_buffer(size_t cap);
int  sio_out_overflowed(void);

/* Ends capture and hands the buffer over; the caller frees it, or passes it
 * to sio_in_from_buffer which takes ownership. NUL-terminated. */
char *sio_take_buffer(size_t *len);

/* Redirect to a file until sio_reset. Returns 0 on success. */
int  sio_out_to_file(const char *path, int append);

/* ---- stdin ---- */

/* One line without its newline. Returns the length, or -1 at end of input --
 * which is also what an empty stdin gives on the first call, so a tool with no
 * input does nothing rather than hanging. */
int  sio_read_line(char *buf, size_t size);

/* Takes ownership of `buf`, which must be NUL-terminated and freeable. */
void sio_in_from_buffer(char *buf);

int  sio_in_from_file(const char *path);

/* Is there any stdin at all? A tool that can work on either its arguments or
 * its input needs to know which it was given. */
int  sio_has_input(void);

#endif /* CARDOS_SIO_H */
