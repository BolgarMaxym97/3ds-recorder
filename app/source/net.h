/*
 * One plain-HTTP GET of the Worker (STATS_URL) at a time, for a background thread: the in-app
 * update (update.c) and Help's support links (support.c). httpcInit/httpcExit stay with the caller.
 */
#ifndef NET_H
#define NET_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool (*NetSink)(const u8 *data, u32 len, void *ctx);

typedef struct {
    LightLock lock;           /* ctx while it is open */
    httpcContext ctx;
    bool open;
    volatile bool *stop, *cancel;   /* either set: no new request, the transfer ends */
    volatile u32 *result;           /* the failing Result, when there is one */
} NetConn;

void net_conn_init(NetConn *c, volatile bool *stop, volatile bool *cancel, volatile u32 *result);

/* One GET of STATS_URL `path` from byte `from` on; the body goes to `sink`. The HTTP status in
 * *status (0: no answer); the header `hdr` (may be NULL) into hdrBuf. False when the transfer did
 * not finish (cancelled, lost, refused by the sink, or not a 200/206). */
bool net_get(NetConn *c, const char *path, u32 from, NetSink sink, void *ctx, u32 *status, const char *hdr, char *hdrBuf,
             size_t hdrSize);

/* From another thread: ends the transfer under way, if any. */
void net_abort(NetConn *c);

#endif
