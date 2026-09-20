/* The print job: one at a time, on a task of its own.
 *
 * An app hands over a document (printdoc.h markup) and returns to its loop;
 * the job copies it, connects to the printer named in /config/printer.txt,
 * streams the rows, disconnects, and leaves one line of status behind. The
 * shell is a single cooperative loop, and a job takes ten seconds or more --
 * the same reason httpq.c exists.
 *
 * The job runs a row *source*, not a document: printq_run_rows is what a
 * pre-rendered bitmap (from a file, or from the PC) will go through when
 * that arrives. The document form is one such source. */
#ifndef CARDOS_PRINTQ_H
#define CARDOS_PRINTQ_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/sys/printdoc.h"

#define PRINTQ_CONFIG "/config/printer.txt"   /* address, address type, name */

/* Start printing `doc`. Copies it. 0 queued, -1 a job is running, -2 no
 * printer is configured, -3 no memory. Failures after this land in
 * printq_status(). */
int printq_print_doc(const char *doc);

/* Start printing rows from `fn`; `done`, if given, is called on the job's
 * task when it finishes, to free `ctx`. Same return values. */
int printq_print_rows(PrintRowFn fn, void *ctx, void (*done)(void *ctx));

int         printq_busy(void);
/* "" when idle and nothing has happened; otherwise the last thing that did:
 * "connecting", "printing 34%", "printed", "print failed: out of paper". */
const char *printq_status(void);

/* The configured printer, from /config/printer.txt. 0 if set. */
int  printq_printer(uint8_t addr[6], uint8_t *addr_type, char *name, size_t name_size);
int  printq_set_printer(const uint8_t addr[6], uint8_t addr_type, const char *name);
void printq_forget_printer(void);

#endif /* CARDOS_PRINTQ_H */
