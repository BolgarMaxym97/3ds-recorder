#ifndef PREP_H
#define PREP_H
/*
 * "Prepare for the phone": turns a .r3d recording into an MP4 (H.264 + AAC) that any phone
 * plays and saves as it is. Portable C99: the app runs it on the console in background
 * threads, tests/prep/host_prep.c runs the same code on a PC.
 *
 * One pass over the recording, the way tools/r3dconv reads it (no --repair): frames are
 * composed into the 400 x 480 (or 400 x 240, top only) picture r3dconv makes; a video frame
 * goes out when a screen changed, at the time the recorder captured it (a clock in ms, variable
 * frame rate: a still picture costs nothing; see GRID_HZ in prep.c). Audio goes the
 * player's way (audio_place + audio_conceal + EQ), is resampled to 32 kHz and encoded to
 * AAC-LC.
 *
 * Two halves, for two threads (two cores on the console):
 *   prep_produce  reads the recording, decodes the screens, makes each video frame's YUV
 *                 picture and encodes the sound; blocks while the queue is full
 *   prep_consume  encodes the pictures to H.264 and builds the file in memory
 *   prep_write    writes those buffers to the card (a third thread: it mostly waits on the card)
 * A queue of pictures and sound frames sits between them.
 *
 * Files next to the recording, for base = ".../20261105_113901":
 *   base.mp4        the result, when done
 *   base.mp4.part   ftyp + mdat being written
 *   base.ptbl       sample table so far (12 bytes per sample)
 *   base.prep       resume points: two slots, the newer valid one wins
 * A resume point is taken before every key frame (every PREP_GOP video frames), with the
 * queue empty, so a job that is stopped, crashes or loses power goes on from there and loses
 * at most one GOP.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PREP_GOP 120   /* key frame every 120 video frames (4 s at 30 fps) */

typedef struct {
    int videoKbps;           /* 0: PREP_VIDEO_KBPS (prep.c) */
    int audioKbps;           /* 0: 96 */
    uint64_t (*nowUs)(void); /* a clock for prep_stats (optional) */
    void (*sleepUs)(uint32_t us); /* waits on the queue (required) */
} PrepOptions;

typedef struct {
    uint32_t frames;       /* video frames encoded by this job (not counting a resumed part) */
    uint32_t audioFrames;  /* AAC frames made by this job */
    uint32_t resumedFrom;  /* video frames that were already done when the job opened */
    /* producer (ms) */
    double readMs, decodeMs, yuvMs, pcmMs, audioMs, checkpointMs, produceWaitMs, diskMs; /* diskMs: MP4 writes */
    /* consumer (ms) */
    double videoMs, writeMs, consumeWaitMs;
} PrepStats;

enum { PREP_MORE = 0, PREP_DONE = 1, PREP_ERROR = -1, PREP_WAIT = 2 };

typedef struct PrepJob PrepJob;

/* Opens `src` (a .r3d) for preparation into base.*: goes on from base.prep when it is valid
   for this recording, starts over otherwise. NULL on error (message in err). */
PrepJob *prep_open(const char *src, const char *base, const PrepOptions *opt, char *err, size_t errLen);
/* The producer: runs until the whole recording is queued (PREP_DONE), an error, or
   prep_abort. Blocks (opt.sleepUs) while the queue is full. One thread. */
int prep_produce(PrepJob *j);
/* The consumer, on another thread: handles what is queued, a few items at a time.
   PREP_MORE: call again; PREP_WAIT: the queue is empty (sleep a little); PREP_DONE: base.mp4
   is there; PREP_ERROR: see prep_error. */
int prep_consume(PrepJob *j);
/* The writer, on a third thread: runs until the file is complete, an error, or prep_abort. */
int prep_write(PrepJob *j);
/* Makes prep_produce return soon (the job keeps its last resume point). */
void prep_abort(PrepJob *j);
/* 0..1000, by the position in the recording */
uint32_t prep_permille(const PrepJob *j);
const PrepStats *prep_stats(const PrepJob *j);
const char *prep_error(const PrepJob *j);
/* Frees the job; prep_produce and prep_write must have returned. The work since the last resume point is
   redone on the next prep_open. */
void prep_close(PrepJob *j);

/* base.<ext> into out (ext: "mp4", "mp4.part", "ptbl", "prep") */
void prep_path(char *out, size_t n, const char *base, const char *ext);
/* What base.prep says: permille done, or -1 when there is no valid resume point. */
int prep_saved_permille(const char *base);

#ifdef __cplusplus
}
#endif
#endif /* PREP_H */
