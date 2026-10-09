/* What the heap looks like, as lines of text. Device-only.
 *
 * One list for the console's `mem` and the built-in Memory app, so the two
 * cannot disagree. The Memory app used to show the handle arena's own
 * statistics -- a subsystem nothing allocates from -- and so reported 0K
 * while `mem` showed the heap that was actually running out.
 *
 * Each line is at most MEMREPORT_LINE - 1 characters and fits the 40-column
 * screen. */
#ifndef CARDOS_MEMREPORT_H
#define CARDOS_MEMREPORT_H

#define MEMREPORT_LINE 41

void mem_report(void (*out)(const char *line, void *ctx), void *ctx);

#endif /* CARDOS_MEMREPORT_H */
