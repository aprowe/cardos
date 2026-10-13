/* MP3 to mono samples. See mp3stream.h. */
#include "kernel/sys/mp3stream.h"

#include <string.h>

/* Layer III only (no MPEG audio in the wild is I or II), and the plain C
 * paths: the host and the device then run the same arithmetic. */
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_EXTERNAL_SCRATCH      /* its 16 KB from the heap: see mp3_play */
#define MINIMP3_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "kernel/ext/minimp3/minimp3.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define IN_CAP 2048                   /* a frame is at most 1441 bytes */

typedef struct {
  mp3dec_t dec;
  uint8_t  in[IN_CAP];
  int16_t  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} Work;

size_t mp3_work_bytes(void) { return sizeof(Work); }
size_t mp3_scratch_bytes(void) { return sizeof(mp3dec_scratch_t); }

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint32_t mp3_id3v2_size(const uint8_t *h, int n) {
  if (n < 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3') return 0;
  if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0;           /* not syncsafe */
  return 10 + ((uint32_t)h[6] << 21 | (uint32_t)h[7] << 14 | (uint32_t)h[8] << 7 | h[9]) +
         ((h[5] & 0x10) ? 10 : 0);
}

/* A whole read, or as much as there is. */
static int read_full(const Mp3Src *src, uint8_t *buf, int n) {
  int got = 0, r;
  while (got < n && (r = src->read(src->ctx, buf + got, n - got)) > 0) got += r;
  return got;
}

/* The Xing (VBR) or Info (LAME's CBR) header inside the first frame: just
 * after the side information, whose size depends on the version and the
 * channels. 1 found. */
static int xing(const uint8_t *f, int n, Mp3Info *out) {
  int mpeg1 = HDR_TEST_MPEG1(f) != 0, mono = HDR_IS_MONO(f);
  int at = 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
  uint32_t flags;
  if (at + 8 > n) return 0;
  if (memcmp(f + at, "Xing", 4) && memcmp(f + at, "Info", 4)) return 0;
  flags = be32(f + at + 4);
  at += 8;
  if (flags & 1) { if (at + 4 > n) return 0; out->frames = be32(f + at); at += 4; }
  if (flags & 2) at += 4;                                      /* bytes: the file says */
  if (flags & 4) {
    if (at + 100 > n) return 1;
    memcpy(out->toc, f + at, 100);
    out->has_toc = 1;
  }
  return 1;
}

int mp3_probe(const Mp3Src *src, Mp3Info *out, uint8_t *buf, int bufn, const char **why) {
  uint32_t start = 0, tag;
  int n, samples, at, fbytes, free_fmt = 0;
  const uint8_t *hdr;
  uint8_t tail[3];

  memset(out, 0, sizeof *out);
  if (why) *why = "not an MP3";
  if (bufn < 4096 || src->seek(src->ctx, 0) != 0) return -1;
  n = read_full(src, buf, 10);
  /* Tags can follow tags. */
  while ((tag = mp3_id3v2_size(buf, n)) != 0) {
    start += tag;
    if (start >= src->size || src->seek(src->ctx, start) != 0) return -1;
    n = read_full(src, buf, 10);
  }
  if (src->seek(src->ctx, start) != 0) return -1;
  n = read_full(src, buf, bufn);
  if (n < 4) return -1;

  /* The first frame minimp3 believes in -- several in a row agreeing, not
   * just an 0xFFF that happens to be there. */
  /* Found the way the decoder finds it, but not by decoding: that keeps
   * 16 KB of scratch and a 6.7 KB decoder on the stack, and this runs on
   * the shell's (it overflowed the first time it ran on the device). */
  at = mp3d_find_frame(buf, n, &free_fmt, &fbytes);
  if (!fbytes || at + 4 > n) return -1;
  hdr = buf + at;
  if (HDR_GET_LAYER(hdr) != 1) { if (why) *why = "MPEG audio, but not layer III"; return -1; }
  samples = (int)hdr_frame_samples(hdr);
  out->first = start + (uint32_t)at;
  out->rate = hdr_sample_rate_hz(hdr);
  out->channels = HDR_IS_MONO(hdr) ? 1 : 2;
  out->kbps = (int)hdr_bitrate_kbps(hdr);
  if (out->rate < 8000) return -1;

  /* An ID3v1 tag is the last 128 bytes. */
  out->end = src->size;
  if (src->size >= 128 + out->first && src->seek(src->ctx, src->size - 128) == 0 &&
      read_full(src, tail, 3) == 3 && !memcmp(tail, "TAG", 3))
    out->end = src->size - 128;

  if (xing(hdr, n - at, out)) {
    /* That frame is the header, not sound: start after it. */
    out->first += (uint32_t)fbytes;
  }
  if (out->frames)
    out->ms = (uint32_t)((uint64_t)out->frames * (uint32_t)samples * 1000 / out->rate);
  else if (out->kbps)
    out->ms = (uint32_t)((uint64_t)(out->end - out->first) * 8 / (uint32_t)out->kbps);
  if (!out->ms) return -1;
  if (why) *why = "";
  return 0;
}

uint32_t mp3_offset_for(const Mp3Info *m, uint32_t ms) {
  uint32_t span = m->end - m->first;
  if (!m->ms || ms >= m->ms) return m->end;
  if (m->has_toc) {
    uint32_t p1000 = (uint32_t)((uint64_t)ms * 100000 / m->ms);    /* percent, x1000 */
    uint32_t pct = p1000 / 1000, frac = p1000 % 1000;
    uint32_t a = m->toc[pct], b = pct < 99 ? m->toc[pct + 1] : 256;
    uint32_t pos = a * 1000 + (b > a ? (b - a) * frac : 0);        /* of 256000 */
    return m->first + (uint32_t)((uint64_t)span * pos / 256000);
  }
  return m->first + (uint32_t)((uint64_t)span * ms / m->ms);
}

int mp3_play(const Mp3Src *src, const Mp3Info *m, void *work, void *scratch, const Mp3Hooks *h) {
  Work *w = work;
  mp3dec_frame_info_t fi;
  uint32_t at = m->first, base_ms = 0;
  uint64_t done = 0;                          /* samples since base_ms */
  int have = 0, eof = 0, starved = 0;

  mp3dec_scratch = scratch;
  mp3dec_init(&w->dec);
  if (src->seek(src->ctx, at) != 0) return -1;
  for (;;) {
    int32_t to;
    int ns, used;
    if (h->stop && h->stop()) return 0;
    /* A seek before the pause: scrubbing while paused moves the place. */
    if (h->seek && (to = h->seek()) >= 0) {
      if ((uint32_t)to > m->ms) to = (int32_t)m->ms;
      at = mp3_offset_for(m, (uint32_t)to);
      if (src->seek(src->ctx, at) != 0) return -1;
      mp3dec_init(&w->dec);                   /* the bit reservoir is the old place's */
      have = 0; eof = 0;
      base_ms = (uint32_t)to; done = 0;
      if (h->progress) h->progress(base_ms);
    }
    if (h->paused && h->paused()) {
      if (h->hush && h->hush(h->ctx) < 0) return -1;
      continue;
    }
    /* Keep the buffer at least half full, in big reads (the card likes
     * them): a frame wants the next one's header behind it. */
    if (!eof && (have < IN_CAP / 2 || starved)) {
      int want = IN_CAP - have, r;
      if ((uint32_t)want > m->end - at) want = (int)(m->end - at);
      r = want > 0 ? src->read(src->ctx, w->in + have, want) : 0;
      if (r < 0) return -1;
      if (r == 0) eof = 1;
      have += r; at += (uint32_t)r;
    }
    if (!have) return 0;
    ns = mp3dec_decode_frame(&w->dec, w->in, have, w->pcm, &fi);
    used = fi.frame_bytes;
    if (!ns && !used) {
      /* Not enough for a frame: the end, or (a full buffer) not audio. */
      if (eof) return 0;
      if (have >= IN_CAP) have = 0;
      starved = 1;
      continue;
    }
    starved = 0;
    if (used > have) used = have;
    memmove(w->in, w->in + used, (size_t)(have - used));
    have -= used;
    if (!ns) continue;                         /* skipped junk */
    if (fi.channels == 2) {
      int i;
      for (i = 0; i < ns; i++)
        w->pcm[i] = (int16_t)(((int32_t)w->pcm[2 * i] + w->pcm[2 * i + 1]) / 2);
    }
    if (h->out(h->ctx, w->pcm, ns) < 0) return -1;
    done += (uint32_t)ns;
    if (h->progress) h->progress(base_ms + (uint32_t)(done * 1000 / m->rate));
  }
}
