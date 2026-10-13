/* MP3, from a byte source to mono samples: what Music plays.
 *
 * The decoding is minimp3 (kernel/ext/minimp3, public domain), layer III
 * only and no SIMD, on host and device alike. This file is what goes round
 * it, and is portable, so the host suite runs it against real files
 * (test/fixtures/mp3_*.mp3): finding the audio behind an ID3v2 tag, the
 * length (from a Xing/Info header when there is one -- a VBR file's bitrate
 * says nothing about its length -- else from the bitrate), a time as a
 * byte offset for seeking (through the Xing table of contents when there
 * is one), and the play loop: read, decode a frame, mix stereo down to one
 * channel -- the speaker is one channel -- and hand it on.
 *
 * kernel/sys/audio.c is the device glue: the card as the source, the
 * speaker as the sink, on the audio task.
 *
 * Memory, all the caller's, in two blocks so a heap in pieces can still
 * give them: the work area (mp3_work_bytes, about 13 KB: the decoder, a
 * 2 KB input buffer and a frame of samples) and minimp3's scratch
 * (mp3_scratch_bytes, about 16 KB) -- which upstream keeps on the stack;
 * the copy here is patched to take it (MINIMP3_EXTERNAL_SCRATCH). On the
 * device that put 35 KB in one piece, more than the heap had with an app
 * open. One decoder at a time.
 */
#ifndef CARDOS_MP3STREAM_H
#define CARDOS_MP3STREAM_H

#include <stddef.h>
#include <stdint.h>

/* Where the bytes come from: a file, read on from where the last seek
 * left it. read returns the bytes read, 0 at the end, < 0 failed. */
typedef struct {
  void    *ctx;
  int    (*read)(void *ctx, uint8_t *buf, int n);
  int    (*seek)(void *ctx, uint32_t off);          /* 0 ok */
  uint32_t size;                                    /* the whole file */
} Mp3Src;

typedef struct {
  uint32_t first;       /* the first audio frame (past ID3v2 and a Xing frame) */
  uint32_t end;         /* the end of the audio (before an ID3v1 tag) */
  uint32_t rate;        /* Hz */
  int      channels;    /* in the file; what comes out is always mono */
  int      kbps;        /* the first frame's */
  uint32_t frames;      /* from a Xing/Info header, or 0 */
  uint32_t ms;          /* the length */
  int      has_toc;
  uint8_t  toc[100];    /* Xing: the byte position, of 256, at each 1% of time */
} Mp3Info;

/* The size of an ID3v2 tag from its first 10 bytes, header included; 0 if
 * these are not one. */
uint32_t mp3_id3v2_size(const uint8_t *h, int n);

/* Read enough of `src` to fill `out`. `buf` is scratch of at least 4 KB.
 * 0, or -1 with *why set ("not an MP3", ...). Leaves the source anywhere. */
int mp3_probe(const Mp3Src *src, Mp3Info *out, uint8_t *buf, int bufn, const char **why);

/* Where in the file `ms` is. */
uint32_t mp3_offset_for(const Mp3Info *m, uint32_t ms);

/* What the play loop asks, between frames. Any may be NULL but out. */
typedef struct {
  void    *ctx;
  int    (*out)(void *ctx, int16_t *mono, int n);  /* n samples at info.rate; 0 ok, < 0 stop */
  int    (*hush)(void *ctx);                       /* paused: some silence, paced; 0 ok */
  int    (*stop)(void);                            /* non-zero: stop now */
  int    (*paused)(void);                          /* non-zero: hold the place */
  int32_t (*seek)(void);                           /* a time in ms to go to, or -1 */
  void   (*progress)(uint32_t ms);                 /* how far, after each frame */
} Mp3Hooks;

size_t mp3_work_bytes(void);
size_t mp3_scratch_bytes(void);

/* Play from the start to the end, a stop, or a failure. `work` is
 * mp3_work_bytes() and `scratch` mp3_scratch_bytes() of the caller's. 0
 * played (or stopped), -1 the source or the sink failed. */
int mp3_play(const Mp3Src *src, const Mp3Info *m, void *work, void *scratch, const Mp3Hooks *h);

#endif /* CARDOS_MP3STREAM_H */
