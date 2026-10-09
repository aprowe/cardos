/* What a sync against a server's list may delete.
 *
 * Music and Photos keep on the card what the server lists and delete what
 * it no longer lists. The list arrives in one fixed buffer, and the HTTP
 * layer fills that buffer and stops: a reply that filled it is not the
 * whole list, and neither is a parse that stopped at the app's maximum. A
 * track past the cut is still on the server, and deleting it costs a
 * download of megabytes once the list fits again -- which is what Music did
 * with a list of more than 64 tracks.
 *
 * So: read the reply with sync_reply, which drops a line the buffer cut in
 * half and says whether the list is whole; delete only when
 * sync_may_delete says so; and keep what a partial list does not mention.
 *
 * Header-only, static helpers, the same arrangement as apps/safefile.h.
 * Host-tested in test/test_syncset.c, and through the apps in
 * test/test_music.c and test/test_photo.c.
 */
#ifndef CARDOS_SYNCSET_H
#define CARDOS_SYNCSET_H

#if defined(__GNUC__)
#define SYNC_OPT __attribute__((unused))
#else
#define SYNC_OPT
#endif

/* A reply of `len` bytes -- what api->http returned -- in a buffer of
 * `cap`. 1 when it came whole. 0 when it filled the buffer, and then the
 * text after the last newline is cut off, since that line was cut too: an
 * id cut in half names a track that does not exist. `buf` is left
 * NUL-terminated at what may be parsed. A failed request (len < 0) is an
 * empty, partial list. */
static SYNC_OPT int sync_reply(char *buf, int len, int cap) {
  if (cap <= 0) return 0;
  if (len < 0) { buf[0] = 0; return 0; }
  if (len < cap - 1) { buf[len] = 0; return 1; }
  len = cap - 1;
  while (len > 0 && buf[len - 1] != '\n') len--;
  buf[len] = 0;
  return 0;
}

/* May a sync delete what the list does not name? Only when the reply was
 * whole and the parse took fewer than its maximum -- a list of exactly
 * `max` might have had more behind it. */
static SYNC_OPT int sync_may_delete(int whole, int parsed, int max) {
  return whole && parsed < max;
}

/* Is `id` one of the `n` ids at `ids`, each `stride` bytes apart? */
static SYNC_OPT int sync_listed(const char *id, const char *ids, int n, int stride) {
  int i;
  for (i = 0; i < n; i++) {
    const char *a = id, *b = ids + (long)i * stride;
    while (*a && *a == *b) { a++; b++; }
    if (*a == *b) return 1;
  }
  return 0;
}

#endif /* CARDOS_SYNCSET_H */
