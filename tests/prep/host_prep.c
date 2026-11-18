/*
 * Runs prep/ (the console's "prepare for the phone") on a PC: the producer and the writer on
 * their own threads, the consumer here, as the app does.
 * Usage: host_prep <file.r3d> <out base> [--interrupt N] [--threads N]
 *   --interrupt N: abort and reopen the job every N consumer calls (resume test)
 *   --threads N:   H.264 in N slices at once (PrepOptions.threads; the New 3DS app uses 2)
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

/* PrepOptions.runJobs: job 0 here, the others on threads of their own, as the app runs job 1 on a
   second core */
typedef struct { void (*job)(void *); void *data; } Slice;
static void *slice_main(void *arg)
{
    Slice *s = arg;
    s->job(s->data);
    return NULL;
}
static void run_jobs(void *token, void (*job)(void *), void *data[], int n)
{
    (void)token;
    pthread_t t[4];
    Slice sl[4];
    for (int i = 1; i < n && i < 4; i++) {
        sl[i].job = job;
        sl[i].data = data[i];
        pthread_create(&t[i], NULL, slice_main, &sl[i]);
    }
    job(data[0]);
    for (int i = 1; i < n && i < 4; i++)
        pthread_join(t[i], NULL);
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
    int every = 0, threads = 1;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--interrupt"))
            every = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--threads"))
            threads = atoi(argv[i + 1]);
    }
    PrepOptions o = {0, 0, now_us, sleep_us, threads, run_jobs, NULL};
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
