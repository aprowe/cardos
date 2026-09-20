/* The print job task. See printq.h. */
#include "kernel/sys/printq.h"
#include "kernel/sys/conf.h"
#include "kernel/sys/applog.h"
#include "kernel/drv/btprint.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "printq";

/* PrintDoc is about 2 KB and lives on this stack, plus the packet buffers
 * and NimBLE's callbacks. Measured usage TBD; 6 KB has headroom. */
#define J_STACK    6144
#define J_PRIORITY 4
#define CONNECT_MS 15000

typedef struct {
  PrintRowFn fn;
  void      *ctx;
  void     (*done)(void *ctx);
  int      (*count)(void *ctx);   /* rows to come, for the percentage; or NULL */
  int        total;
} Job;

/* The document form of a job: the copied text and its renderer. */
typedef struct {
  char    *text;
  PrintDoc doc;
} DocJob;

static volatile int s_busy;
/* Written by the job task, read by the shell. A torn read shows a partial
 * line for one frame; the final byte is always NUL. Not worth a lock. */
static char s_status[64];

static void set_status(const char *fmt, ...) {
  char tmp[sizeof s_status];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  memcpy(s_status, tmp, sizeof s_status);
  s_status[sizeof s_status - 1] = 0;
}

static void job_task(void *param) {
  Job *j = (Job *)param;
  uint8_t addr[6], type;
  uint8_t pkt[192];
  uint8_t row[PRINT_ROW_BYTES];
  int n, sent = 0, ok = 0;
  const char *why = NULL;

  if (printq_printer(addr, &type, NULL, 0) != 0) { why = "no printer set"; goto out; }
  /* Counted here rather than by the caller: it renders the whole document
   * once, on this stack, not the shell's. */
  if (j->count) j->total = j->count(j->ctx);

  set_status("connecting");
  if (btprint_connect(addr, type, CONNECT_MS) != 0) { why = btprint_error(); goto out; }

  n = printdoc_prologue(pkt, sizeof pkt);
  if (n < 0 || btprint_write(pkt, (size_t)n) != 0) { why = btprint_error(); goto out; }

  while (j->fn(j->ctx, row)) {
    n = printdoc_row_packet(row, pkt, sizeof pkt);
    if (btprint_write(pkt, (size_t)n) != 0) { why = btprint_error(); goto out; }
    sent++;
    if (j->total > 0 && (sent & 15) == 0)
      set_status("printing %d%%", sent * 100 / j->total);
  }

  n = printdoc_epilogue(pkt, sizeof pkt);
  if (n < 0 || btprint_write(pkt, (size_t)n) != 0) { why = btprint_error(); goto out; }

  /* Give the status reply a moment to arrive, then read it: the difference
   * between "printed" and "printed nothing, out of paper". */
  vTaskDelay(pdMS_TO_TICKS(300));
  {
    uint8_t reply[32];
    int rn = btprint_last_reply(reply, sizeof reply);
    const char *st = printdoc_status_text(reply, (size_t)rn);
    if (st && strcmp(st, "ok") != 0) { why = st; goto out; }
  }
  ok = 1;

out:
  btprint_disconnect();
  if (ok) {
    set_status("printed");
    applogf("print", "printed %d rows", sent);
  } else {
    set_status("print failed: %s", why ? why : "?");
    applogf("print", "failed after %d rows: %s", sent, why ? why : "?");
  }
  ESP_LOGI(TAG, "%s", s_status);
  if (j->done) j->done(j->ctx);
  free(j);
  s_busy = 0;
  vTaskDelete(NULL);
}

static int start(PrintRowFn fn, void *ctx, void (*done)(void *ctx),
                 int (*count)(void *ctx)) {
  Job *j;
  uint8_t addr[6], type;
  if (s_busy) return -1;
  if (printq_printer(addr, &type, NULL, 0) != 0) return -2;
  j = (Job *)calloc(1, sizeof *j);
  if (!j) return -3;
  j->fn = fn; j->ctx = ctx; j->done = done; j->count = count;
  s_busy = 1;
  set_status("starting");
  if (xTaskCreatePinnedToCore(job_task, "print", J_STACK, j, J_PRIORITY,
                              NULL, 0) != pdPASS) {
    free(j);
    s_busy = 0;
    set_status("print failed: no memory for the task");
    return -3;
  }
  return 0;
}

int printq_print_rows(PrintRowFn fn, void *ctx, void (*done)(void *ctx)) {
  return start(fn, ctx, done, NULL);
}

static int doc_rows(void *ctx, uint8_t row[PRINT_ROW_BYTES]) {
  return printdoc_next_row(&((DocJob *)ctx)->doc, row);
}

static int doc_count(void *ctx) {
  return printdoc_count_rows(((DocJob *)ctx)->text);
}

static void doc_done(void *ctx) {
  DocJob *d = (DocJob *)ctx;
  free(d->text);
  free(d);
}

int printq_print_doc(const char *doc) {
  DocJob *d;
  int rc;
  if (!doc) return -3;
  if (s_busy) return -1;
  d = (DocJob *)calloc(1, sizeof *d);
  if (!d) return -3;
  d->text = strdup(doc);
  if (!d->text) { free(d); return -3; }
  printdoc_begin(&d->doc, d->text);
  rc = start(doc_rows, d, doc_done, doc_count);
  if (rc != 0) doc_done(d);
  return rc;
}

int printq_busy(void) { return s_busy; }
const char *printq_status(void) { return s_status; }

/* ------------------------------------------------------------- config -- */

int printq_printer(uint8_t addr[6], uint8_t *addr_type, char *name, size_t name_size) {
  char lines[3][32];
  if (conf_read(PRINTQ_CONFIG, &lines[0][0], 3, sizeof lines[0]) < 1) return -1;
  if (btprint_parse_addr(lines[0], addr) != 0) return -1;
  if (addr_type) *addr_type = (uint8_t)atoi(lines[1]);
  if (name && name_size) snprintf(name, name_size, "%s", lines[2]);
  return 0;
}

int printq_set_printer(const uint8_t addr[6], uint8_t addr_type, const char *name) {
  char a[20], t[4];
  const char *values[3];
  btprint_format_addr(addr, a, sizeof a);
  snprintf(t, sizeof t, "%u", (unsigned)addr_type);
  values[0] = a; values[1] = t; values[2] = name ? name : "";
  return conf_write(PRINTQ_CONFIG, values, 3);
}

void printq_forget_printer(void) { conf_remove(PRINTQ_CONFIG); }
