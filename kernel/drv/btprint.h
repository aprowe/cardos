/* Bluetooth LE client for the "cat printer" family of thermal printers
 * (GB01/GT01/MX06/X6h -- the ones the TinyPrint app drives). Device-only.
 *
 * One link, no pairing: the printer wants neither encryption nor a bond,
 * and asking would only give it something to refuse. Service 0xAE30,
 * commands go to 0xAE01 write-without-response, status comes back as
 * notifications on 0xAE02. Bytes are the packet format in
 * kernel/sys/printdoc.h; this file knows nothing about what they mean.
 *
 * Everything here blocks by polling, because it is called from the print
 * job's own task (kernel/sys/printq.c) and never from the shell loop.
 * Discovery itself runs from GATT callbacks like bthid.c's does, for the
 * same reason: nothing waits on a lock that a callback needs.
 *
 * Shares the radio with bthid.c (bt_radio_up) and takes the third of
 * CONFIG_BT_NIMBLE_MAX_CONNECTIONS, so a mouse and a keyboard can stay. */
#ifndef CARDOS_BTPRINT_H
#define CARDOS_BTPRINT_H

#include <stddef.h>
#include <stdint.h>

#define BTPRINT_SCAN_MAX 8

typedef struct {
  uint8_t addr[6];          /* as NimBLE holds it: least significant byte first */
  uint8_t addr_type;
  int8_t  rssi;
  char    name[24];
} BtPrintSeen;

/* Look for printers for `seconds`. A device counts when it advertises the
 * 0xAE30 or 0xAF30 service, or a name the family uses. Results stay until
 * the next scan; returns how many, or -1 if the radio would not start. */
int  btprint_scan(int seconds);
int  btprint_scan_count(void);
const BtPrintSeen *btprint_scan_result(int i);

/* Open a link and find the two characteristics. Returns 0 once commands can
 * be written, -1 otherwise, with the reason in btprint_error(). Blocks up to
 * `timeout_ms`. */
int  btprint_connect(const uint8_t addr[6], uint8_t addr_type, int timeout_ms);
void btprint_disconnect(void);
int  btprint_connected(void);

/* Write `n` bytes of packets to the printer, split at what the link takes.
 * Blocks; retries when NimBLE is out of buffers rather than dropping bytes,
 * which is the flow control write-without-response does not have. Returns
 * 0, or -1 if the link died. */
int  btprint_write(const uint8_t *buf, size_t n);

/* The last notification the printer sent, raw, and its length; 0 if none
 * since connect. For printdoc_status_text. */
int  btprint_last_reply(uint8_t *out, size_t size);

const char *btprint_error(void);

/* Parse "aa:bb:cc:dd:ee:ff" (as printed, most significant first) into the
 * byte order NimBLE wants. 0 on success. */
int  btprint_parse_addr(const char *text, uint8_t addr[6]);
void btprint_format_addr(const uint8_t addr[6], char *out, size_t size);

#endif /* CARDOS_BTPRINT_H */
