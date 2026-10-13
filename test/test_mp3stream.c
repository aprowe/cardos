/* MP3 to mono samples: kernel/sys/mp3stream.c, against real files.
 *
 * test/fixtures/mp3_cbr_mono.mp3: 2 s of 440 Hz, 44.1 kHz mono, 64 kbps,
 * an ID3v2 tag (title Sine, artist Test Tone) and LAME's Info frame.
 * test/fixtures/mp3_vbr_stereo.mp3: 3 s of 1 kHz, 22.05 kHz stereo VBR,
 * no tag, a Xing frame with a table of contents. Both made by ffmpeg. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "kernel/sys/mp3stream.h"

typedef struct {
  uint8_t *data;
  uint32_t size, at;
  int      reads;
} Mem;

static int mem_read(void *ctx, uint8_t *buf, int n) {
  Mem *m = ctx;
  uint32_t left = m->size - m->at;
  if ((uint32_t)n > left) n = (int)left;
  memcpy(buf, m->data + m->at, (size_t)n);
  m->at += (uint32_t)n;
  m->reads++;
  return n;
}
static int mem_seek(void *ctx, uint32_t off) {
  Mem *m = ctx;
  if (off > m->size) return -1;
  m->at = off;
  return 0;
}

static int load(const char *name, Mem *m, Mp3Src *src) {
  char path[512];
  const char *f = __FILE__, *cut = strrchr(f, '/'), *bs = strrchr(f, '\\');
  FILE *fp;
  long n;
  if (bs && (!cut || bs > cut)) cut = bs;
  if (cut) snprintf(path, sizeof path, "%.*sfixtures/%s", (int)(cut - f + 1), f, name);
  else snprintf(path, sizeof path, "test/fixtures/%s", name);
  memset(m, 0, sizeof *m);
  if ((fp = fopen(path, "rb")) == NULL) return -1;
  fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
  m->data = malloc((size_t)n);
  m->size = (uint32_t)fread(m->data, 1, (size_t)n, fp);
  fclose(fp);
  src->ctx = m; src->read = mem_read; src->seek = mem_seek; src->size = m->size;
  return 0;
}

/* What the sink saw. */
static struct {
  int      n, calls, crossings, peak;
  int16_t  last;
  int      fail_after;
  int      stop_at;            /* samples: stop once this many have gone */
  int32_t  seek_to;            /* asked for once, then -1 */
  int      seek_after;         /* ...after this many samples */
  uint32_t progress;
} S;

static int sink(void *ctx, int16_t *pcm, int n) {
  int i;
  (void)ctx;
  if (S.fail_after && S.calls >= S.fail_after) return -1;
  S.calls++;
  for (i = 0; i < n; i++) {
    int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (v > S.peak) S.peak = v;
    /* Crossings with hysteresis, so dither in a quiet patch is not a tone. */
    if ((S.last >= 0 && pcm[i] < -300) || (S.last < 0 && pcm[i] > 300)) {
      S.crossings++;
      S.last = pcm[i];
    }
  }
  S.n += n;
  return 0;
}
static int stop_cb(void) { return S.stop_at && S.n >= S.stop_at; }
static int32_t seek_cb(void) {
  int32_t t;
  if (S.seek_to < 0 || S.n < S.seek_after) return -1;
  t = S.seek_to; S.seek_to = -1;
  return t;
}
static void progress_cb(uint32_t ms) { S.progress = ms; }

static void reset_sink(void) { memset(&S, 0, sizeof S); S.seek_to = -1; }

static Mp3Hooks hooks(void) {
  Mp3Hooks h;
  memset(&h, 0, sizeof h);
  h.out = sink; h.stop = stop_cb; h.seek = seek_cb; h.progress = progress_cb;
  return h;
}

void test_mp3_id3v2_size_counts_the_header_and_syncsafe_bytes(void) {
  const uint8_t tag[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0x01, 0x7F };
  const uint8_t footer[10] = { 'I', 'D', '3', 4, 0, 0x10, 0, 0, 0, 10 };
  const uint8_t no[10] = { 0xFF, 0xFB, 0x90, 0 };
  CHECK_EQ(mp3_id3v2_size(tag, 10), 10 + 128 + 127);
  CHECK_EQ(mp3_id3v2_size(footer, 10), 10 + 10 + 10);   /* a footer is ten more */
  CHECK_EQ(mp3_id3v2_size(no, 10), 0);
  CHECK_EQ(mp3_id3v2_size(tag, 9), 0);
}

void test_mp3_probe_a_cbr_file_behind_a_tag(void) {
  Mem m; Mp3Src src; Mp3Info info; uint8_t buf[4096];
  const char *why = "";
  CHECK_EQ(load("mp3_cbr_mono.mp3", &m, &src), 0);
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), 0);
  CHECK_EQ(info.rate, 44100);
  CHECK_EQ(info.channels, 1);
  CHECK_EQ(info.kbps, 64);
  CHECK(info.first > mp3_id3v2_size(m.data, 10));      /* past the tag and the Info frame */
  CHECK(info.frames > 0);                              /* LAME's Info frame counts them */
  CHECK(info.ms >= 1990 && info.ms <= 2100);
  CHECK_EQ(info.end, m.size);
  free(m.data);
}

void test_mp3_probe_a_vbr_file_reads_the_xing_frame(void) {
  Mem m; Mp3Src src; Mp3Info info; uint8_t buf[4096];
  const char *why = "";
  CHECK_EQ(load("mp3_vbr_stereo.mp3", &m, &src), 0);
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), 0);
  CHECK_EQ(info.rate, 22050);
  CHECK_EQ(info.channels, 2);
  CHECK(info.has_toc);
  CHECK(info.ms >= 2990 && info.ms <= 3100);
  free(m.data);
}

void test_mp3_probe_refuses_what_is_not_one(void) {
  uint8_t junk[3000], buf[4096];
  Mem m; Mp3Src src; Mp3Info info;
  const char *why = "";
  int i;
  for (i = 0; i < (int)sizeof junk; i++) junk[i] = (uint8_t)(i * 7);
  memcpy(junk, "RIFF", 4);
  memset(&m, 0, sizeof m);
  m.data = junk; m.size = sizeof junk;
  src.ctx = &m; src.read = mem_read; src.seek = mem_seek; src.size = m.size;
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), -1);
  CHECK(why[0] != 0);
}

void test_mp3_offset_for_is_inside_the_audio_and_grows(void) {
  Mem m; Mp3Src src; Mp3Info info; uint8_t buf[4096];
  const char *why;
  uint32_t a, b, c;
  CHECK_EQ(load("mp3_vbr_stereo.mp3", &m, &src), 0);
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), 0);
  a = mp3_offset_for(&info, 0);
  b = mp3_offset_for(&info, info.ms / 2);
  c = mp3_offset_for(&info, info.ms * 2);              /* past the end: the end */
  CHECK_EQ(a, info.first);
  CHECK(b > a && b < info.end);
  CHECK(c <= info.end && c > b);
  free(m.data);
}

void test_mp3_play_mono_comes_out_as_the_tone_it_was(void) {
  Mem m; Mp3Src src; Mp3Info info; uint8_t buf[4096];
  const char *why;
  Mp3Hooks h = hooks();
  void *work = malloc(mp3_work_bytes()), *scr = malloc(mp3_scratch_bytes());
  CHECK_EQ(load("mp3_cbr_mono.mp3", &m, &src), 0);
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), 0);
  reset_sink();
  CHECK_EQ(mp3_play(&src, &info, work, scr, &h), 0);
  /* Two seconds, give or take the encoder's padding frames. */
  CHECK(S.n >= 44100 * 2 - 1152 && S.n <= 44100 * 2 + 3 * 1152);
  /* 440 Hz crosses zero 880 times a second. */
  CHECK(S.crossings > 1700 && S.crossings < 1820);
  CHECK(S.peak > 1000);
  CHECK(S.progress >= 1990);
  free(work); free(scr); free(m.data);
}

void test_mp3_play_stereo_is_mixed_down_to_one_channel(void) {
  Mem m; Mp3Src src; Mp3Info info; uint8_t buf[4096];
  const char *why;
  Mp3Hooks h = hooks();
  void *work = malloc(mp3_work_bytes()), *scr = malloc(mp3_scratch_bytes());
  CHECK_EQ(load("mp3_vbr_stereo.mp3", &m, &src), 0);
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), 0);
  reset_sink();
  CHECK_EQ(mp3_play(&src, &info, work, scr, &h), 0);
  /* Samples, not samples times two: three seconds at 22050. */
  CHECK(S.n >= 22050 * 3 - 576 && S.n <= 22050 * 3 + 4 * 576);
  CHECK(S.crossings > 5900 && S.crossings < 6100);
  free(work); free(scr); free(m.data);
}

void test_mp3_play_stops_when_asked(void) {
  Mem m; Mp3Src src; Mp3Info info; uint8_t buf[4096];
  const char *why;
  Mp3Hooks h = hooks();
  void *work = malloc(mp3_work_bytes()), *scr = malloc(mp3_scratch_bytes());
  CHECK_EQ(load("mp3_cbr_mono.mp3", &m, &src), 0);
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), 0);
  reset_sink();
  S.stop_at = 10000;
  CHECK_EQ(mp3_play(&src, &info, work, scr, &h), 0);
  CHECK(S.n >= 10000 && S.n < 10000 + 1152 + 1);
  free(work); free(scr); free(m.data);
}

void test_mp3_play_seeks_and_progress_follows(void) {
  Mem m; Mp3Src src; Mp3Info info; uint8_t buf[4096];
  const char *why;
  Mp3Hooks h = hooks();
  void *work = malloc(mp3_work_bytes()), *scr = malloc(mp3_scratch_bytes());
  CHECK_EQ(load("mp3_cbr_mono.mp3", &m, &src), 0);
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), 0);
  reset_sink();
  S.seek_to = 1500;
  S.seek_after = 4000;
  CHECK_EQ(mp3_play(&src, &info, work, scr, &h), 0);
  /* About 0.1 s before the jump, then the last half second. */
  CHECK(S.n > 4000 + 44100 / 2 - 3 * 1152 && S.n < 4000 + 1152 + 44100 / 2 + 3 * 1152);
  /* A seek by bytes lands within a frame or two of the time asked. */
  CHECK(S.progress + 150 >= info.ms);
  free(work); free(scr); free(m.data);
}

void test_mp3_play_a_sink_that_fails_ends_it(void) {
  Mem m; Mp3Src src; Mp3Info info; uint8_t buf[4096];
  const char *why;
  Mp3Hooks h = hooks();
  void *work = malloc(mp3_work_bytes()), *scr = malloc(mp3_scratch_bytes());
  CHECK_EQ(load("mp3_cbr_mono.mp3", &m, &src), 0);
  CHECK_EQ(mp3_probe(&src, &info, buf, sizeof buf, &why), 0);
  reset_sink();
  S.fail_after = 3;
  CHECK_EQ(mp3_play(&src, &info, work, scr, &h), -1);
  CHECK_EQ(S.calls, 3);
  free(work); free(scr); free(m.data);
}
