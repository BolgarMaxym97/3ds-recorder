#include "sdio.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static FS_Archive g_sdmc;
static bool g_open;

Result sdio_init(void)
{
    if (g_open) {
        return 0;
    }
    Result rc = fsInit();
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = FSUSER_OpenArchive(&g_sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(rc)) {
        fsExit();
        return rc;
    }
    g_open = true;
    /* Already-exists errors are expected and ignored. */
    FSUSER_CreateDirectory(g_sdmc, fsMakePath(PATH_ASCII, "/3ds"), 0);
    FSUSER_CreateDirectory(g_sdmc, fsMakePath(PATH_ASCII, "/3ds/recorder"), 0);
    FSUSER_CreateDirectory(g_sdmc, fsMakePath(PATH_ASCII, BENCH_DIR), 0);
    return 0;
}

void sdio_exit(void)
{
    if (g_open) {
        FSUSER_CloseArchive(g_sdmc);
        fsExit();
        g_open = false;
    }
}

FS_Archive sdio_archive(void)
{
    return g_sdmc;
}

static void flush(sdio_text_t *t)
{
    if (t->used == 0 || t->error) {
        t->used = 0;
        return;
    }
    u32 written = 0;
    Result rc = FSFILE_Write(t->file, &written, t->offset, t->buf, t->used, FS_WRITE_FLUSH);
    if (R_FAILED(rc)) {
        t->error = rc;
    }
    t->offset += written;
    t->used = 0;
}

bool sdio_text_open(sdio_text_t *t, const char *path, bool append, bool *created)
{
    memset(t, 0, offsetof(sdio_text_t, buf));
    FS_Path p = fsMakePath(PATH_ASCII, path);
    if (!append) {
        FSUSER_DeleteFile(g_sdmc, p);
    }
    u64 size = 0;
    Result rc = FSUSER_OpenFile(&t->file, g_sdmc, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_FAILED(rc)) {
        t->error = rc;
        return false;
    }
    FSFILE_GetSize(t->file, &size);
    t->offset = size;
    if (created) {
        *created = size == 0;
    }
    return true;
}

void sdio_text_printf(sdio_text_t *t, const char *fmt, ...)
{
    if (!t->file) {
        return;
    }
    char line[2048]; /* the game4_*.csv header alone is ~950 chars */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if ((u32)n >= sizeof(line)) {
        n = sizeof(line) - 1;
    }
    if (t->used + (u32)n > sizeof(t->buf)) {
        flush(t);
    }
    memcpy(t->buf + t->used, line, (u32)n);
    t->used += (u32)n;
}

void sdio_text_close(sdio_text_t *t)
{
    if (!t->file) {
        return;
    }
    flush(t);
    FSFILE_Close(t->file);
    t->file = 0;
}
