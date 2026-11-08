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

#include "app.h"
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
        app_update(&app, &in);
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
        C2D_TargetClear(top, COL_BG);
        scene_begin(top, TOP_W);
        app_draw_top(&app);
        C2D_TargetClear(bottom, COL_BG);
        scene_begin(bottom, BOTTOM_W);
        app_draw_bottom(&app);
        C3D_FrameEnd(0);
    }

    app_exit(&app);
    ui_exit();
    C2D_Fini();
    C3D_Fini();
    FSUSER_CloseArchive(sd);
    gfxExit();
    romfsExit();
    return 0;
}
