/*
 * Runs prep/ (the console's "prepare for the phone") on a PC: the producer and the writer on
 * their own threads, the consumer here, as the app does.
 * Usage: host_prep <file.r3d> <out base> [--interrupt N]
 *   --interrupt N: abort and reopen the job every N consumer calls (resume test)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "prep.h"

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void sleep_us(uint32_t us)
{
    usleep(us);
}

static void *producer(void *job)
{
    prep_produce(job);
    return NULL;
}

static void *writer(void *job)
{
    prep_write(job);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.r3d> <out base> [--interrupt N]\n", argv[0]);
        return 2;
    }
    const int every = argc > 4 && !strcmp(argv[3], "--interrupt") ? atoi(argv[4]) : 0;
    PrepOptions o = {0, 0, now_us, sleep_us};
    char err[128];
    int opens = 0, r = PREP_MORE;
    const uint64_t t0 = now_us();
    while (r != PREP_DONE) {
        PrepJob *j = prep_open(argv[1], argv[2], &o, err, sizeof(err));
        if (!j) {
            fprintf(stderr, "open: %s\n", err);
            return 1;
        }
        opens++;
        if (prep_stats(j)->resumedFrom)
            fprintf(stderr, "resumed at frame %u (%u permille)\n", prep_stats(j)->resumedFrom, prep_permille(j));
        pthread_t th, tw;
        pthread_create(&th, NULL, producer, j);
        pthread_create(&tw, NULL, writer, j);
        int calls = 0;
        for (;;) {
            r = prep_consume(j);
            if (r == PREP_WAIT)
                usleep(500);
            if (r == PREP_DONE || r == PREP_ERROR || (every && ++calls >= every))
                break;
        }
        prep_abort(j);
        pthread_join(th, NULL);
        pthread_join(tw, NULL);
        const PrepStats *s = prep_stats(j);
        fprintf(stderr, "%u video + %u audio frames; read %.0f decode %.0f yuv %.0f pcm %.0f audio %.0f ckpt %.0f "
                        "pwait %.0f disk %.0f | video %.0f write %.0f cwait %.0f ms\n",
                s->frames, s->audioFrames, s->readMs, s->decodeMs, s->yuvMs, s->pcmMs, s->audioMs, s->checkpointMs,
                s->produceWaitMs, s->diskMs, s->videoMs, s->writeMs, s->consumeWaitMs);
        if (r == PREP_ERROR) {
            fprintf(stderr, "error: %s\n", prep_error(j));
            prep_close(j);
            return 1;
        }
        prep_close(j);
    }
    fprintf(stderr, "done in %.1f s, %d open(s)\n", (now_us() - t0) / 1e6, opens);
    return 0;
}
