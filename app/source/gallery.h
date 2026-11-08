/*
 * Gallery data: the recordings on the SD card, their details and preview frames. A background
 * thread does all SD work (listing, reading each file's header and R3DE end record, decoding the
 * first keyframes, deleting); the UI reads the results under gal_lock().
 */
#ifndef GALLERY_H
#define GALLERY_H

#include <3ds.h>
#include <stdbool.h>

#define GAL_THUMB_W 64   /* list thumbnails: a 64 x 32 RGB565 texture, 50 x 30 of it drawn */
#define GAL_THUMB_H 32
#define GAL_THUMB_DRAWN_W 50
#define GAL_THUMB_DRAWN_H 30
#define GAL_LAST_W 128   /* the panel's last recording: 128 x 64, 100 x 60 of it drawn */
#define GAL_LAST_H 64
#define GAL_TEX_W 512    /* full frames: 512 x 256 RGB565 textures (400 or 320 x 240 used) */
#define GAL_TEX_H 256

typedef struct {
    char name[32];        /* yyyymmdd_hhmmss.r3d */
    u64 bytes;
    /* filled in by the thread; read only when `info` is set */
    bool info;
    bool ended;           /* has its R3DE end record: closed properly */
    bool readable;        /* header valid */
    bool audio, both;
    u8 fps;
    u32 durationMs;       /* from R3DE, or estimated from the file's last records */
    u32 dropped;
    /* list thumbnail, GPU tiled RGB565 (GAL_THUMB_W x GAL_THUMB_H); NULL until decoded */
    u16 *thumb;
    bool thumbFailed;
} GalItem;

typedef struct {
    bool loaded;          /* the directory has been read */
    int count;
    GalItem *items;       /* newest first */
    u64 bytes;
    u32 version;          /* bumps on each change, so the UI knows to re-read */
} GalList;

/* The selected recording's first frames, GPU tiled (GAL_TEX_W x GAL_TEX_H each). */
typedef struct {
    char name[32];        /* which recording; empty: none yet */
    bool ok;              /* decoded; false: could not be read */
    bool hasBottom;
    u16 *top, *bottom;
    u32 version;
} GalPreview;

void gal_start(void);     /* starts the thread, which reads the list */
void gal_stop(void);
void gal_lock(void);
void gal_unlock(void);
const GalList *gal_list(void);          /* under gal_lock() */
const GalPreview *gal_preview(void);    /* under gal_lock() */
void gal_want_preview(const char *name);
/* Deletes a recording (asynchronous); gal_take_deleted() then reports how it went. */
void gal_delete(const char *name);
bool gal_take_deleted(bool *ok);
/* The panel's last recording, any thread (it reads the SD card): its first top frame shrunk to
   100 x 60 in a tiled GAL_LAST_W x GAL_LAST_H texture. */
bool gal_last_thumb(const char *name, u16 *thumb);
/* Fills in it->readable ... dropped from the file named it->name (it->bytes set); any thread. */
void gal_read_info(GalItem *it);
/* Holds the thread's SD work (while the player reads the card). */
void gal_hold(bool hold);

#endif
