/*
 * recprobe launcher (phase S spike): starts the recprobe sysmodule once and
 * shows the last result. Run from the Homebrew Launcher.
 *   A: start recprobe     START: exit
 * The module keeps running after this app exits (pm only ends dependency-
 * loaded processes together with their parent).
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "diag_addr.h"
#include "recprobe_diag.h"

#define RECPROBE_TID 0x0004013000F1D302ULL
#define CXI_PATH     "/luma/sysmodules/0004013000F1D302.cxi"
#define INFO_PATH    "/3ds/recorder/spike/info.txt"

Result svcControlService(u32 op, ...);   /* Luma custom SVCs (csvc.s) */
Result svcMapProcessMemoryEx(Handle dstProcess, u32 destAddress, Handle srcProcess, u32 srcAddress, u32 size, u32 flags);
Result svcUnmapProcessMemoryEx(Handle process, u32 destAddress, u32 size);

/* Opens a process by name; 0 if not running. */
static Handle OpenProcessByName(const char *wanted)
{
    u32 pids[64];
    s32 n = 0;
    if (R_FAILED(svcGetProcessList(&n, pids, 64)))
        return 0;
    for (s32 i = 0; i < n; i++)
    {
        Handle h = 0;
        if (R_FAILED(svcOpenProcess(&h, pids[i])))
            continue;
        char name[9] = { 0 };
        s64 raw = 0;
        if (R_SUCCEEDED(svcGetProcessInfo(&raw, h, 0x10000)))
            memcpy(name, &raw, 8);
        if (strcmp(name, wanted) == 0)
            return h;
        svcCloseHandle(h);
    }
    return 0;
}

/* Reads the module's progress page; returns the map result. */
static Result ReadDiag(Handle module, RecprobeDiag *out)
{
    static const u32 candidates[] = { 0x0E000000, 0x0D000000, 0x0C000000 };
    Result rc = -1;
    for (u32 i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
    {
        MemInfo mi;
        PageInfo pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, candidates[i])) || mi.state != MEMSTATE_FREE)
            continue;
        rc = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, candidates[i], module, RECPROBE_DIAG_VA, 0x1000, 0);
        if (R_FAILED(rc))
            continue;
        memcpy(out, (const void *)candidates[i], sizeof(*out));
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, candidates[i], 0x1000);
        return 0;
    }
    return rc;
}

static void ShowStatus(void)
{
    consoleClear();
    printf("recprobe launcher (phase S spike)\n\n");
    FILE *f = fopen(CXI_PATH, "rb");
    printf("Module file: %s\n", f ? "found" : "MISSING " CXI_PATH);
    if (f)
        fclose(f);
    Handle module = OpenProcessByName("recprobe");
    printf("Module running: %s\n", module ? "YES" : "no");
    if (module)
    {
        RecprobeDiag d;
        memset(&d, 0, sizeof(d));
        const Result rc = ReadDiag(module, &d);
        svcCloseHandle(module);
        if (R_FAILED(rc))
            printf("Diag: map failed 0x%08lX\n", (u32)rc);
        else if (d.magic != RECPROBE_DIAG_MAGIC)
            printf("Diag: bad magic %08lX (old module?)\n", d.magic);
        else
        {
            printf("Stage: %lu %s, srv tries %lu\n", d.stage, recprobe_stage_name(d.stage), d.srvTries);
            printf("rc heap %08lX srv %08lX\n", (u32)d.rc[RECPROBE_RC_HEAP], (u32)d.rc[RECPROBE_RC_SRV]);
            printf("rc fs   %08lX sdmc %08lX\n", (u32)d.rc[RECPROBE_RC_FS], (u32)d.rc[RECPROBE_RC_SDMC]);
            printf("rc mkdir %08lX file %08lX\n", (u32)d.rc[RECPROBE_RC_MKDIR], (u32)d.rc[RECPROBE_RC_FILE]);
            printf("rc mcu %08lX steal %08lX led %08lX\n", (u32)d.rc[RECPROBE_RC_MCU],
                   (u32)d.rc[RECPROBE_RC_MCU_STEAL], (u32)d.rc[RECPROBE_RC_LED]);
            printf("setup %ld, sys free %lu KB, probe %lu KB, ring %lu KB\n", d.rc[RECPROBE_RC_SETUP],
                   d.sysFreeKb, d.probeMaxKb, d.ringKb);
            printf("t %lu s (%s), captures %lu, written %lu KB\n", d.seconds, d.phase ? "REC" : "detect",
                   d.captures, d.writtenKb);
        }
    }
    printf("\n");
    f = fopen(INFO_PATH, "r");
    if (f)
    {
        char line[160];
        printf("Last info.txt:\n");
        while (fgets(line, sizeof(line), f))
            printf("%s", line);
        fclose(f);
    }
    printf("\nA: start recprobe   START: exit (refreshes 1/s)\n");
    printf("Luma option needed: 'Enable loading\nexternal FIRMs and modules'.\n");
}

static void Launch(void)
{
    Result rc = pmAppInit();
    bool stolen = false;
    if (R_FAILED(rc))
    {
        rc = svcControlService(0 /* SERVICEOP_STEAL_CLIENT_SESSION */, pmAppGetSessionHandle(), "pm:app");
        stolen = R_SUCCEEDED(rc);
    }
    printf("\npm:app: 0x%08lX%s\n", (u32)rc, stolen ? " (stolen)" : "");
    if (R_FAILED(rc))
        return;
    const FS_ProgramInfo info = { .programId = RECPROBE_TID, .mediaType = MEDIATYPE_NAND };
    rc = PMAPP_LaunchTitle(&info, 0);
    printf("PMAPP_LaunchTitle: 0x%08lX\n", (u32)rc);
    if (stolen)
        svcCloseHandle(*pmAppGetSessionHandle());
    else
        pmAppExit();
    if (R_SUCCEEDED(rc))
        printf("Started. LED: magenta -> green/red.\nYou can exit now (START).\n");
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    ShowStatus();
    u32 frames = 0;
    while (aptMainLoop())
    {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_START)
            break;
        if (down & KEY_A)
            Launch();
        if (down & KEY_Y)
            ShowStatus();
        if (++frames % 60 == 0)
            ShowStatus();   /* live view */
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
