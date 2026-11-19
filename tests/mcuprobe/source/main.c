/*
 * mcuprobe: does what a HOME Menu battery-percent patch does (issue #10): every 200 ms it opens
 * mcu::HWC, reads the battery level and closes the session again. The MCU's services take one
 * session each, so this fails while another program keeps mcu::HWC open (the recorder module did,
 * and the patch showed "Error" in HOME Menu's status bar).
 *
 * Run it with the recorder on (recording, replay buffer, LED signals): "failed" must stay 0.
 *   START  quits; the totals go to sdmc:/3ds/recorder/mcuprobe.log
 */
#include <3ds.h>

#include <stdio.h>
#include <time.h>

static Result read_level(u8 *pct)
{
    Handle h = 0;
    Result rc = srvGetServiceHandle(&h, "mcu::HWC");
    if (R_FAILED(rc))
        return rc;
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0x5, 0, 0); /* MCUHWC:GetBatteryLevel */
    rc = svcSendSyncRequest(h);
    if (R_SUCCEEDED(rc))
        rc = (Result)cmd[1];
    *pct = (u8)cmd[2];
    svcCloseHandle(h);
    return rc;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    srvSetBlockingPolicy(true); /* a taken port fails at once instead of waiting for ever */
    u32 ok = 0, failed = 0;
    Result last = 0;
    u8 pct = 0;
    u64 next = 0;
    printf("mcuprobe: mcu::HWC open/read/close every 200 ms\nSTART: quit\n\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        if (osGetTime() >= next) {
            next = osGetTime() + 200;
            const Result rc = read_level(&pct);
            if (R_SUCCEEDED(rc))
                ok++;
            else {
                failed++;
                last = rc;
            }
            printf("\x1b[5;0Hbattery %3u%%   ok %6lu   failed %6lu   last error %08lX\x1b[K", pct, ok, failed,
                   (unsigned long)last);
        }
        gspWaitForVBlank();
    }
    FILE *f = fopen("sdmc:/3ds/recorder/mcuprobe.log", "a");
    if (f) {
        const time_t t = time(NULL);
        const struct tm *tm = gmtime(&t);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ok %lu, failed %lu, last error %08lX\n", tm->tm_year + 1900,
                tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, ok, failed, (unsigned long)last);
        fclose(f);
    }
    gfxExit();
    return 0;
}
