/*
 * 3DS Recorder control app (.3dsx and .cia from the same code): installs, starts, controls and
 * uninstalls the rec3ds sysmodule, which is embedded in this app (romfs:/rec3ds.cxi).
 * Design: design/ui-studio.html (variant B).
 */
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app.h"
#include "i18n.h"
#include "lid.h"
#include "prepare.h"
#include "recorder.h"
#include "ui.h"

/* Linear memory: the two double-size render targets (~4 MB), the font sheets (~2 MB) and room
 * for the player's frames later. The rest of the process memory is the main heap. */
u32 __ctru_linear_heap_size = 24 * 1024 * 1024;

/* With only the linear size set, libctru hands everything else to the main heap. A 3dsx runs as
 * 3dsx_app, which on a New 3DS gets ~124 MB instead of the CIA's 64 MB: libctru then asks for a
 * main heap larger than the 96 MB heap area, the kernel refuses and the app stops in svcBreak
 * before main. 3ds-ua's installer (2026-10-28) and 3ds-ua-hub (2026-11-01) both hit it; this is
 * their fix: libctru's own allocation with the main heap capped at the heap area. */
#define HEAP_SIZE_MAX (OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN)
/* The CIA runs in the New 3DS 124 MB mode (cia.rsf: SystemModeExt) and would get a ~96 MB heap;
   it never needed more than the ~39 MB a 64 MB title has, so it keeps that on every console. */
#define HEAP_APP_MAX  (40u * 1024 * 1024)

extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap;
extern char *fake_heap_start, *fake_heap_end;

/* libctru 2.7.0's own __system_allocateHeaps (weak), with the main heap capped. */
void __system_allocateHeaps(void)
{
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
        svcBreak(USERBREAK_PANIC);
    ResourceLimitType type = RESLIMIT_COMMIT;
    s64 max = 0, used = 0;
    svcGetResourceLimitLimitValues(&max, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&used, reslimit, &type, 1);
    svcCloseHandle(reslimit);

    u32 remaining = (u32)(max - used) & ~0xFFF;
    if (__ctru_linear_heap_size > remaining)
        svcBreak(USERBREAK_PANIC);
    __ctru_heap_size = remaining - __ctru_linear_heap_size;
    if (__ctru_heap_size > HEAP_SIZE_MAX)
        __ctru_heap_size = HEAP_SIZE_MAX;
    if (__ctru_heap_size > HEAP_APP_MAX)
        __ctru_heap_size = HEAP_APP_MAX;

    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size, MEMOP_ALLOC,
                                  MEMPERM_READ | MEMPERM_WRITE)))
        svcBreak(USERBREAK_PANIC);
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                                  MEMPERM_READ | MEMPERM_WRITE)))
        svcBreak(USERBREAK_PANIC);

    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

/* The screens are drawn at twice their size and the transfer to the display averages each 2x2
 * block: citro2d has no antialiasing, and on a console the focus ring, thin strokes and text
 * shrunk from the 24 px font come out ragged otherwise (3ds-ua-hub). */
#define SCREEN_TRANSFER                                                                                    \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                       \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |         \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY))

static C3D_RenderTarget *screen_target(gfxScreen_t screen, u16 width)
{
    C3D_RenderTarget *target = C3D_RenderTargetCreate(240 * 2, width * 2, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    if (target)
        C3D_RenderTargetSetOutput(target, screen, GFX_LEFT, SCREEN_TRANSFER);
    return target;
}

static void scene_begin(C3D_RenderTarget *target, u16 width)
{
    C2D_SceneBegin(target);
    C2D_SceneSize(240, width, true);
}

#ifdef UI_TOUR
/* Both screens as one 400x480 BMP (the top one, the bottom one under it centred), read back from
   the framebuffers: 240-pixel columns of B, G, R, the left one first, bottom to top. */
static void tour_save(const char *path)
{
    static u8 img[400 * 480 * 3];
    memset(img, 0, sizeof(img));
    u16 fw, fh;
    const u8 *top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
    const u8 *bot = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fw, &fh);
    for (int y = 0; y < 480; y++) { /* BMP rows: bottom up */
        u8 *row = img + (size_t)(479 - y) * 400 * 3;
        for (int x = 0; x < 400; x++) {
            const u8 *src = NULL;
            if (y < 240)
                src = top + ((size_t)x * 240 + (239 - y)) * 3;
            else if (x >= 40 && x < 360)
                src = bot + ((size_t)(x - 40) * 240 + (239 - (y - 240))) * 3;
            if (src)
                memcpy(row + x * 3, src, 3);
        }
    }
    u8 h[54] = {'B', 'M'};
    const u32 size = sizeof(h) + sizeof(img);
    memcpy(h + 2, &size, 4);
    h[10] = 54, h[14] = 40;
    const u32 w = 400, hh = 480;
    memcpy(h + 18, &w, 4);
    memcpy(h + 22, &hh, 4);
    h[26] = 1, h[28] = 24;
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(h, 1, sizeof(h), f);
        fwrite(img, 1, sizeof(img), f);
        fclose(f);
    }
}

/* the languages to tour: sdmc:/3ds/recorder/tour.txt ("ja zh-Hant"), every one without it */
static int tour_langs(int *out)
{
    int n = 0;
    char buf[128] = "";
    FILE *f = fopen("sdmc:/3ds/recorder/tour.txt", "r");
    if (f) {
        buf[fread(buf, 1, sizeof(buf) - 1, f)] = 0;
        fclose(f);
        for (char *t = strtok(buf, " \r\n,"); t; t = strtok(NULL, " \r\n,"))
            for (int i = 0; i < LANG_COUNT; i++)
                if (!strcasecmp(t, LANGS[i].code))
                    out[n++] = i;
    }
    if (!n)
        for (int i = 0; i < LANG_COUNT; i++)
            out[n++] = i;
    return n;
}
#endif

#ifdef PERF_LOG
/* make PERF=1: the CPU time of drawing a frame (building it: texts parsed, shapes queued; not the
   GPU), per screen and language: average and worst of the first 240 frames on each screen */
static void perf_frame(const App *a, u64 ticks)
{
    static int screen = -1, lang = -1;
    static u32 n;
    static u64 sum, worst;
    if ((int)a->screen != screen || (int)a->cfg.lang != lang) {
        screen = a->screen, lang = a->cfg.lang, n = 0, sum = worst = 0;
        return;
    }
    if (n >= 240)
        return;
    sum += ticks;
    if (ticks > worst)
        worst = ticks;
    if (++n == 240)
        app_log("perf: screen %d, %s: draw %.2f ms average, %.2f ms worst", screen, LANGS[lang].code,
                (double)sum / n / (SYSCLOCK_ARM11 / 1000.0), (double)worst / (SYSCLOCK_ARM11 / 1000.0));
}
#endif

int main(int argc, char **argv)
{
    romfsInit();
    osSetSpeedupEnable(true); /* New 3DS: 804 MHz + L2 (the MP4 encoder); no-op on an Old 3DS */
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
    C2D_Prepare();
    C3D_RenderTarget *top = screen_target(GFX_TOP, TOP_W);
    C3D_RenderTarget *bottom = screen_target(GFX_BOTTOM, BOTTOM_W);

    FS_Archive sd = 0;
    FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    recorder_init(sd);
    app_log_start();

    /* The 3dsx deletes itself on uninstall: the Homebrew Launcher passes its path. */
    const char *self = argc > 0 && argv[0] && strstr(argv[0], ".3dsx") ? argv[0] : "/3ds/3ds-recorder.3dsx";
    static App app;
    const bool ready = top && bottom && ui_init();
    app_init(&app, self);
    lid_init();

#ifdef UI_TOUR
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    mkdir("sdmc:/3ds/recorder/tour", 0777);
    int tourLang[LANG_COUNT], tourLangs = tour_langs(tourLang), tourL = 0, tourStep = 0, tourWait = -1;
    char tourName[48];
#endif
    bool touching = false, dragged = false;
    u32 idle = 0, skipped = 0; /* frames without input; frames not drawn since the last drawn one */
    touchPosition first = {0, 0}, last = {0, 0};
    while (ready && aptMainLoop() && !app.quit) {
        hidScanInput();
        Input in = {.down = hidKeysDown(), .held = hidKeysHeld()};
        if (hidKeysHeld() & KEY_TOUCH) {
            touchPosition now;
            hidTouchRead(&now);
            if (!touching) {
                first = last = now;
                dragged = false;
            }
            if (abs(now.py - first.py) > 8 || abs(now.px - first.px) > 8)
                dragged = true;
            last = now;
            touching = true;
        } else if (touching) {
            touching = false;
            in.tapped = !dragged; /* a tap: lifted close to where it came down */
            in.tap_x = last.px;
            in.tap_y = last.py;
        }
#ifdef UI_TOUR
        in = (Input){0};
        if (tourWait < 0) {
            tourWait = app_tour(&app, tourLang[tourL], tourStep, tourName, sizeof(tourName));
            if (!tourWait && ++tourL < tourLangs) {
                tourStep = 0;
                tourWait = app_tour(&app, tourLang[tourL], tourStep, tourName, sizeof(tourName));
            }
            if (!tourWait)
                break;
        }
#endif
        app_update(&app, &in);
        if (lid_step(app.cfg.lidWork)) { /* the lid is closed: the screens are dark, the work goes on (lid.h) */
            svcSleepThread(50 * 1000 * 1000);
            continue;
        }
        idle = in.down || in.held || touching || in.tapped ? 0 : idle + 1;
        /* While a recording is converted (prepare.c) and nobody touches the console, the UI draws
           3 times a second instead of 60: drawing takes much of the core the encoder runs on.
           Any key or touch brings 60 back at once. */
        const u32 every = prepare_busy() ? 20 : app.screen == SCR_PHONE ? 6 : 1; /* sending: 10 fps */
        if (every > 1 && idle > 90 && ++skipped < every) {
            gspWaitForVBlank();
            continue;
        }
        skipped = 0;

        ui_frame_begin();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
#ifdef PERF_LOG
        const u64 perfT0 = svcGetSystemTick();
#endif
        C2D_TargetClear(top, COL_BG);
        scene_begin(top, TOP_W);
        app_draw_top(&app);
        C2D_TargetClear(bottom, COL_BG);
        scene_begin(bottom, BOTTOM_W);
        app_draw_bottom(&app);
#ifdef PERF_LOG
        perf_frame(&app, svcGetSystemTick() - perfT0);
#endif
        C3D_FrameEnd(0);
#ifdef UI_TOUR
        if (--tourWait == 0) {
            gspWaitForVBlank();
            gspWaitForVBlank();
            char path[96];
            snprintf(path, sizeof(path), "sdmc:/3ds/recorder/tour/%s-%02d-%s.bmp", LANGS[tourLang[tourL]].code,
                     tourStep, tourName);
            tour_save(path);
            tourStep++;
            tourWait = -1;
        }
#endif
    }

    lid_exit();
    app_exit(&app);
    ui_exit();
    C2D_Fini();
    C3D_Fini();
    FSUSER_CloseArchive(sd);
    gfxExit();
    romfsExit();
    return 0;
}
