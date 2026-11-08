#include "recorder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../../sysmod/source/shared.h"
#include "shared_addr.h"

Result svcControlService(u32 op, ...);
Result svcMapProcessMemoryEx(Handle dstProcess, u32 destAddress, Handle srcProcess, u32 srcAddress, u32 size, u32 flags);
Result svcUnmapProcessMemoryEx(Handle process, u32 destAddress, u32 size);

#define MAP_VA 0x0E000000u

static FS_Archive g_sd;

static LightLock g_logLock;   /* the UI, server and conversion threads all log */
static char g_about[256];

void recorder_init(FS_Archive sd)
{
    g_sd = sd;
    LightLock_Init(&g_logLock);
}
FS_Archive recorder_archive(void) { return g_sd; }

void app_log(const char *fmt, ...)
{
    LightLock_Lock(&g_logLock);
    FILE *f = fopen(REC_APP_LOG, "a");
    if (!f) {
        LightLock_Unlock(&g_logLock);
        return;
    }
    const time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm); /* the console clock is local time */
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03u] ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
            tm.tm_min, tm.tm_sec, (unsigned)(osGetTime() % 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    LightLock_Unlock(&g_logLock);
}

const char *app_about(void) { return g_about; }

void app_log_start(void)
{
    struct stat st;
    if (stat(REC_APP_LOG, &st) == 0 && st.st_size > REC_LOG_MAX) { /* the previous part is kept once */
        remove(REC_APP_LOG_OLD);
        rename(REC_APP_LOG, REC_APP_LOG_OLD);
    }
    u8 model = 0xFF, region = 0xFF;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemModel(&model);
        CFGU_SecureInfoGetRegion(&region);
        cfguExit();
    }
    static const char *const models[] = {"Old 3DS", "Old 3DS XL", "New 3DS", "Old 2DS", "New 3DS XL", "New 2DS XL"};
    static const char *const regions[] = {"JPN", "USA", "EUR", "AUS", "CHN", "KOR", "TWN"};
    const u32 luma = recorder_luma_version(), kv = osGetKernelVersion(), fv = osGetFirmVersion();
    extern u32 __ctru_heap_size;
    snprintf(g_about, sizeof(g_about),
             "app %s%s%s (%s) | %s (model %u) | %s | Luma %lu.%lu.%lu | kernel %lu.%lu-%lu, firm %lu.%lu-%lu | "
             "heap %lu KB, linear free %lu KB",
             APP_VERSION, APP_CHANNEL[0] ? " " : "", APP_CHANNEL, envIsHomebrew() ? "3dsx" : "cia", model < 6 ? models[model] : "unknown model",
             (unsigned)model, region < 7 ? regions[region] : "unknown region", (unsigned long)(luma >> 16),
             (unsigned long)(luma >> 8 & 0xFF), (unsigned long)(luma & 0xFF), (unsigned long)GET_VERSION_MAJOR(kv),
             (unsigned long)GET_VERSION_MINOR(kv), (unsigned long)GET_VERSION_REVISION(kv),
             (unsigned long)GET_VERSION_MAJOR(fv), (unsigned long)GET_VERSION_MINOR(fv),
             (unsigned long)GET_VERSION_REVISION(fv), (unsigned long)(__ctru_heap_size / 1024),
             (unsigned long)(linearSpaceFree() / 1024));
    s64 mhz = 0, l2 = 0; /* New 3DS only (Luma): did the speed-up take? */
    if (R_SUCCEEDED(svcGetSystemInfo(&mhz, 0x10001, 0)) && R_SUCCEEDED(svcGetSystemInfo(&l2, 0x10001, 2))) {
        const size_t n = strlen(g_about);
        snprintf(g_about + n, sizeof(g_about) - n, " | %lld MHz, L2 %s", (long long)mhz, l2 ? "on" : "off");
    }
    app_log("=== app start: %s", g_about);
}

/* ------------------------------------------------------------------ module process */

static Handle OpenModule(void)
{
    u32 pids[64];
    s32 n = 0;
    if (R_FAILED(svcGetProcessList(&n, pids, 64)))
        return 0;
    for (s32 i = 0; i < n; i++) {
        Handle h = 0;
        if (R_FAILED(svcOpenProcess(&h, pids[i])))
            continue;
        char name[9] = {0};
        s64 raw = 0;
        if (R_SUCCEEDED(svcGetProcessInfo(&raw, h, 0x10000)))
            memcpy(name, &raw, 8);
        if (strcmp(name, REC_PROCESS_NAME) == 0)
            return h;
        svcCloseHandle(h);
    }
    return 0;
}

static bool ModuleRunning(void)
{
    Handle h = OpenModule();
    if (h)
        svcCloseHandle(h);
    return h != 0;
}

/* Maps the module's shared page at MAP_VA; caller unmaps. */
static volatile RecShared *MapShared(Handle module)
{
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, MAP_VA)) || mi.state != MEMSTATE_FREE)
        return NULL;
    if (R_FAILED(svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, MAP_VA, module, REC_SHARED_VA, 0x1000, 0)))
        return NULL;
    volatile RecShared *s = (volatile RecShared *)MAP_VA;
    if (s->magic != REC_SHARED_MAGIC) {
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, MAP_VA, 0x1000);
        return NULL;
    }
    return s;
}

static void UnmapShared(void) { svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, MAP_VA, 0x1000); }

bool recorder_take_system_stop(char *name, size_t cap)
{
    const FS_Path p = fsMakePath(PATH_ASCII, REC_DIR_FS "/.system_stop");
    Handle f = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, g_sd, p, FS_OPEN_READ, 0)))
        return false;
    char buf[48] = {0};
    u32 got = 0;
    FSFILE_Read(f, &got, 0, buf, sizeof(buf) - 1);
    FSFILE_Close(f);
    FSUSER_DeleteFile(g_sd, p);
    buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = 0;
    snprintf(name, cap, "%s", buf);
    return got > 0;
}

int recorder_take_recovered(char *name, size_t cap)
{
    int n = 0;
    Handle m = OpenModule();
    if (!m)
        return 0;
    volatile RecShared *s = MapShared(m);
    if (s && s->version >= 2 && s->recovered) { /* an older module has no such field */
        n = (int)s->recovered;
        snprintf(name, cap, "%.31s", (const char *)s->recoveredFile);
        s->recovered = 0;
    }
    if (s)
        UnmapShared();
    svcCloseHandle(m);
    return n;
}

void recorder_status(RecStatus *out)
{
    memset(out, 0, sizeof(*out));
    Handle m = OpenModule();
    if (!m)
        return;
    out->running = true;
    volatile RecShared *s = MapShared(m);
    if (s) {
        out->readable = true;
        out->state = s->state;
        out->recSeconds = s->recSeconds;
        out->recKb = s->recKb;
        out->framesTop = s->framesTop;
        out->framesBot = s->framesBot;
        out->dropped = s->dropped;
        out->files = s->files;
        out->lastError = s->lastError;
        snprintf(out->lastFile, sizeof(out->lastFile), "%s", (const char *)s->lastFile);
        UnmapShared();
    }
    svcCloseHandle(m);
}

bool recorder_command(u32 cmd)
{
    Handle m = OpenModule();
    if (!m)
        return false;
    volatile RecShared *s = MapShared(m);
    if (s) {
        s->cmd = cmd;
        UnmapShared();
    }
    svcCloseHandle(m);
    return s != NULL;
}

static Result WithPmApp(Result (*fn)(void *), void *arg)
{
    Result rc = pmAppInit();
    const bool stolen = R_FAILED(rc);
    if (stolen)
        rc = svcControlService(0 /* steal client session */, pmAppGetSessionHandle(), "pm:app");
    if (R_FAILED(rc))
        return rc;
    rc = fn(arg);
    if (stolen)
        svcCloseHandle(*pmAppGetSessionHandle());
    else
        pmAppExit();
    return rc;
}

static Result LaunchFn(void *arg)
{
    (void)arg;
    const FS_ProgramInfo info = {.programId = REC_TITLE_ID, .mediaType = MEDIATYPE_NAND};
    return PMAPP_LaunchTitle(&info, 0);
}

static Result TerminateFn(void *arg)
{
    (void)arg;
    return PMAPP_TerminateTitle(REC_TITLE_ID, 3ull * 1000 * 1000 * 1000);
}

bool recorder_stop(void)
{
    if (!ModuleRunning())
        return true;
    recorder_command(REC_CMD_EXIT);
    for (int i = 0; i < 150; i++) { /* up to 15 s: a recording is saved first */
        svcSleepThread(100 * 1000 * 1000);
        if (!ModuleRunning())
            return true;
    }
    WithPmApp(TerminateFn, NULL);
    svcSleepThread(500 * 1000 * 1000);
    return !ModuleRunning();
}

/* ------------------------------------------------------------------ files */

static bool FileExists(const char *path)
{
    Handle f = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, g_sd, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return false;
    FSFILE_Close(f);
    return true;
}

ModuleFile recorder_module_file(void)
{
    FILE *a = fopen("romfs:/rec3ds.cxi", "rb");
    FILE *b = fopen("sdmc:" REC_CXI_FS, "rb");
    ModuleFile st = b ? MODULE_DIFFERENT : MODULE_MISSING;
    /* heap buffers: the info thread and the action worker can both compare */
    u8 *ba = a && b ? malloc(16384) : NULL, *bb = ba ? malloc(16384) : NULL;
    if (ba && bb) {
        st = MODULE_SAME;
        for (;;) {
            const size_t na = fread(ba, 1, 16384, a), nb = fread(bb, 1, 16384, b);
            if (na != nb || memcmp(ba, bb, na) != 0) {
                st = MODULE_DIFFERENT;
                break;
            }
            if (na == 0)
                break;
        }
    }
    free(ba);
    free(bb);
    if (a)
        fclose(a);
    if (b)
        fclose(b);
    return st;
}

static bool InstallModule(void)
{
    FSUSER_CreateDirectory(g_sd, fsMakePath(PATH_ASCII, "/luma"), 0);
    FSUSER_CreateDirectory(g_sd, fsMakePath(PATH_ASCII, "/luma/sysmodules"), 0);
    FILE *in = fopen("romfs:/rec3ds.cxi", "rb");
    FILE *out = fopen("sdmc:" REC_CXI_FS, "wb");
    bool ok = in && out;
    static u8 buf[16384];
    while (ok) {
        const size_t n = fread(buf, 1, sizeof(buf), in);
        if (!n)
            break;
        ok = fwrite(buf, 1, n, out) == n;
    }
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    if (!ok)
        FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, REC_CXI_FS));
    return ok;
}

bool recorder_luma_modules_on(void)
{
    s64 cfg = 0;
    if (R_FAILED(svcGetSystemInfo(&cfg, 0x10000, 3)))
        return false;
    return (cfg >> 1) & 1; /* singleOptions: LOADEXTFIRMSANDMODULES = 1 */
}

u32 recorder_luma_version(void)
{
    s64 v = 0;
    if (R_FAILED(svcGetSystemInfo(&v, 0x10000, 0)))
        return 0;
    return (u32)v >> 8; /* major << 24 | minor << 16 | build << 8 */
}

Result recorder_install_and_start(const char **step)
{
    *step = "";
    app_log("start: checking the module file");
    if (recorder_module_file() != MODULE_SAME) {
        app_log("start: installing the module");
        if (ModuleRunning() && !recorder_stop()) {
            *step = "stop";
            return -1;
        }
        if (!InstallModule()) {
            *step = "install";
            return -2;
        }
    }
    if (ModuleRunning())
        return 0;
    if (!recorder_luma_modules_on()) {
        *step = "luma";
        return -3;
    }
    *step = "start";
    app_log("start: launching");
    const Result rc = WithPmApp(LaunchFn, NULL);
    app_log("start: launched, rc %08lX", (unsigned long)rc);
    return rc;
}

void recorder_files(RecFiles *out)
{
    memset(out, 0, sizeof(*out));
    Handle dir = 0;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, g_sd, fsMakePath(PATH_ASCII, REC_FILES_FS))))
        return;
    static FS_DirectoryEntry e[16];
    u32 n = 0;
    while (R_SUCCEEDED(FSDIR_Read(dir, &n, 16, e)) && n > 0) {
        for (u32 i = 0; i < n; i++) {
            if ((e[i].attributes & FS_ATTRIBUTE_DIRECTORY) || strcmp(e[i].shortExt, "R3D") != 0)
                continue;
            char name[48];
            u32 k = 0;
            for (; k + 1 < sizeof(name) && e[i].name[k]; k++)
                name[k] = (char)e[i].name[k]; /* names are ASCII: yyyymmdd_hhmmss.r3d */
            name[k] = 0;
            out->count++;
            out->bytes += e[i].fileSize;
            if (strcmp(name, out->newest) > 0) {
                snprintf(out->newest, sizeof(out->newest), "%s", name);
                out->newestBytes = e[i].fileSize;
            }
        }
    }
    FSDIR_Close(dir);
}

u32 recorder_sd_free_mb(void)
{
    FS_ArchiveResource r;
    if (R_FAILED(FSUSER_GetArchiveResource(&r, SYSTEM_MEDIATYPE_SD)))
        return 0;
    return (u32)((u64)r.freeClusters * r.clusterSize / (1024 * 1024));
}

bool recorder_uninstall(bool recordings, const char *selfPath)
{
    recorder_stop();
    FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, REC_CXI_FS));
    FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, REC_DIR_FS "/config.ini"));
    static const char *const files[] = {REC_DIR_FS "/recorder.log", REC_DIR_FS "/recorder.old.log", REC_DIR_FS "/.system_stop",
                                        REC_DIR_FS "/app.log", REC_DIR_FS "/app.old.log",
                                        REC_FILES_FS "/.next.tmp", REC_FILES_FS "/.next.info"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, files[i]));
    if (recordings)
        FSUSER_DeleteDirectoryRecursively(g_sd, fsMakePath(PATH_ASCII, REC_FILES_FS));
    else
        FSUSER_DeleteDirectory(g_sd, fsMakePath(PATH_ASCII, REC_FILES_FS)); /* only if empty */
    FSUSER_DeleteDirectory(g_sd, fsMakePath(PATH_ASCII, REC_DIR_FS));       /* only if empty */
    bool appLeft = false;
    /* The app removes itself last. romfs keeps the .3dsx (or the CIA's content) open, and an
       open file cannot be deleted. */
    romfsExit();
    if (envIsHomebrew()) {
        const char *self = strncmp(selfPath, "sdmc:", 5) == 0 ? selfPath + 5 : selfPath;
        FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, self));
        appLeft = FileExists(self);
    } else {
        u64 tid = 0;
        if (R_SUCCEEDED(APT_GetProgramID(&tid)) && R_SUCCEEDED(amInit())) {
            appLeft = R_FAILED(AM_DeleteTitle(MEDIATYPE_SD, tid));
            amExit();
        } else
            appLeft = true;
    }
    return !appLeft && !FileExists(REC_CXI_FS);
}
