/*
 * "To phone": a small HTTP server on the console for one recording, the one picked in the gallery
 * (port 8080, up to SRV_CLIENTS requests at once, one thread, non-blocking sockets + poll; after
 * tests/wifibench). The QR code opens <address>/?f=<name>. It serves
 *   GET /               the converter page (romfs:/web/index.html.gz, sent gzip-encoded)
 *   GET /api/rec        that recording: name, size, length, screens, sound, closed properly,
 *                       plus the page's language, the app version and the console
 *   GET /rec/<name>     the recording, with Range (the page downloads 2-3 pieces at once)
 *   GET /rec/<stem>.mp4 the MP4 the console prepared from it (prepare.c), once it is ready;
 *                       /api/rec says how far that is ("prep", "prepPct" in 1/1000, "mp4" bytes)
 *   GET /api/log?m=...  a line from the page into app.log (what the phone did: remote testing)
 *   GET /log.txt        diagnostics to send to the developer: the console line, config.ini, the
 *                       recorder's file names and sizes, Luma crash dump names, the tails of both logs
 * Any other name is 404: no other file on the SD card can be read.
 */
#ifndef SERVER_H
#define SERVER_H

#include <3ds.h>
#include <stdbool.h>

#define SRV_PORT 8080
#define SRV_CLIENTS 8

typedef enum { SRV_OFF, SRV_NO_WIFI, SRV_FAILED, SRV_RUNNING } ServerState;

typedef struct {
    ServerState state;
    char address[32];     /* "http://192.168.1.20:8080" */
    char url[80];         /* what the QR code holds: address/?f=name */
    int connections;      /* requests being served now */
    char file[32];        /* the recording last requested; empty: none yet */
    u64 fileSize;
    u64 fileDone;         /* bytes of it sent (over all its pieces) */
    u32 kbPerSec;         /* over the last few seconds */
    u32 lastActivityMs;   /* osGetTime() & 0xFFFFFFFF of the last request or byte sent */
} ServerStats;

/* name: the recording to offer; lang: "en" / "uk" for the page; version and console are shown on it. */
ServerState server_start(const char *name, const char *lang, const char *version, bool new3ds);
void server_stop(void);
void server_stats(ServerStats *out);

#endif
