/*
 * The player: plays a .r3d on the console. A thread reads the file in large chunks, decodes the
 * XR32 frames into a small queue per screen and feeds the sound to the DSP (ndsp) through the same
 * processing as r3dconv (placement on the timeline, lost frames filled in, the treble cut). The
 * sound is the clock when there is any: video shows the frame due at the samples played so far.
 * Frames reach their textures by a GX display transfer (linear -> tiled), not the CPU.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#define PLAYER_TEX_W 256   /* a screen as the LCD stores it: 240 rows per column, 400 or 320 columns */
#define PLAYER_TEX_H 512

/* durationMs: the recording's length if known (gallery), 0 otherwise. False: could not open it. */
bool player_start(const char *name, u32 durationMs);
void player_stop(void);

void player_toggle_pause(void);
void player_seek(s64 targetMs);         /* clamped to the recording */
void player_set_mute(bool mute);

bool player_paused(void);
bool player_ended(void);
bool player_muted(void);
bool player_has_audio(void);            /* the file has sound and the DSP plays it */
bool player_file_audio(void);           /* the file has sound (whether or not it plays) */
bool player_both(void);                 /* both screens recorded */
bool player_bottom_only(void);          /* only the bottom screen recorded */
u32 player_time_ms(void);
u32 player_duration_ms(void);

/* After C3D_FrameBegin: copies the frames now due into the textures (PLAYER_TEX_W x PLAYER_TEX_H,
   GPU_RGB565). Returns the screens that have a picture (bit 0 top, bit 1 bottom). */
int player_present(C3D_Tex *top, C3D_Tex *bottom);

#endif
