/*
 * "Share statistics" (Settings, on by default): when the app starts, a background thread sends
 * the log lines written since the last time - app.log and recorder.log, the newest 16 + 32 KB at
 * most - with the console model, versions and config.ini, and then at most one new Luma crash
 * dump. To the Worker in stats/ (Cloudflare D1), over plain HTTP: the console's TLS does not get
 * through to workers.dev (3ds-ua's net test, 2026-10-23).
 *
 * Silent by design: no Wi-Fi, the setting off, or any error - nothing is shown, the lines are
 * sent with the next launch that gets through. Only a 2xx moves the marks in stats.ini, so what
 * was sent is not sent again.
 */
#ifndef STATS_H
#define STATS_H

#include <stdbool.h>

#ifndef STATS_URL
#define STATS_URL "http://3ds-recorder-stats.bolgar-maxym.workers.dev"
#endif
#define STATS_INI "sdmc:/3ds/recorder/stats.ini"
/* httpc's shared memory, the same for every httpcInit in the app (stats.c, update.c): libctru counts
   the inits and only the first one sets it, and a POST needs room for its whole body (96 KB) */
#define NET_HTTPC_SHM (128 * 1024)

/* Starts the thread when enabled (config.ini stats=on). */
void stats_start(bool enabled);
/* The setting changed: off stops a send that has not posted yet. */
void stats_set_enabled(bool enabled);
/* This console's id ("K7QM-29XD"), made on first use; "" if the SD card refuses the file. */
const char *stats_id(void);
/* App exit: cancels a request in flight and waits for the thread. */
void stats_stop(void);

#endif
