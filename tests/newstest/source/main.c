/*
 * newstest: makes the notification LED events of issue #3 ("a StreetPass / SpotPass notification
 * seems to stop the recording: the red light goes off") on demand, so the recorder can be tested
 * in a minute instead of waiting for a real StreetPass.
 *
 *   A      adds a system notification (news), the way SpotPass software does: the system lights
 *          the notification LED blue over the recorder's red (it comes back within 16 s)
 *   START  quits
 *
 * (Painting the LED directly is left out: mcu::HWC has a single session, the recorder holds it,
 * and asking for it waits for ever, O3DS 2026-11-16.)
 *
 * Every action is written to sdmc:/3ds/recorder/newstest.log with the time, to be put next to the
 * recorder's own log.
 */
#include <3ds.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

static void log_line(const char *what, Result rc)
{
    const time_t t = time(NULL);
    const struct tm *tm = gmtime(&t);
    char line[160];
    snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] %s: rc %08lX\n", tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, what, (unsigned long)rc);
    printf("%s", line);
    FILE *f = fopen("sdmc:/3ds/recorder/newstest.log", "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}

static u32 to_utf16(u16 *out, u32 cap, const char *s)
{
    const ssize_t n = utf8_to_utf16(out, (const u8 *)s, cap - 1);
    out[n > 0 ? n : 0] = 0;
    return n > 0 ? (u32)n : 0;
}

static Result add_notification(void)
{
    Result rc = newsInit();
    if (R_FAILED(rc))
        return rc;
    static u16 title[32], msg[128];
    const u32 tl = to_utf16(title, 32, "3DS Recorder test");
    const u32 ml = to_utf16(msg, 128, "A test notification from newstest (recorder LED test).");
    rc = NEWS_AddNotification(title, tl, msg, ml, NULL, 0, false);
    newsExit();
    return rc;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("newstest (recorder LED test)\n\n");
    printf(" A      add a system notification\n");
    printf(" START  quit\n\n");
    while (aptMainLoop()) {
        hidScanInput();
        const u32 d = hidKeysDown();
        if (d & KEY_START)
            break;
        if (d & KEY_A)
            log_line("system notification added", add_notification());
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
