#ifndef SDIO_H
#define SDIO_H

/*
 * Minimal SD helpers on top of FS:USER. The plugin does not rely on newlib's
 * sdmc: device, so all file output goes through these.
 */

#include <3ds.h>
#include <stdarg.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_DIR "/3ds/recorder/bench"

Result sdio_init(void);           /* fsInit + open SDMC archive + create BENCH_DIR */
void   sdio_exit(void);
FS_Archive sdio_archive(void);

/* Buffered text writer: appends to a file, flushes every 32 KB and on close. */
typedef struct {
    Handle file;
    u64 offset;
    u32 used;
    Result error;
    char buf[32 * 1024];
} sdio_text_t;

bool sdio_text_open(sdio_text_t *t, const char *path, bool append, bool *created);
void sdio_text_printf(sdio_text_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sdio_text_close(sdio_text_t *t);

#ifdef __cplusplus
}
#endif

#endif /* SDIO_H */
