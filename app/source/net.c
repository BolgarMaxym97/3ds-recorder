#include "net.h"

#include <stdio.h>
#include <stdlib.h>

#include "stats.h"

#define CHUNK 0x4000
#define CONNECT_NS (10ull * 1000 * 1000 * 1000)
#define READ_NS (15ull * 1000 * 1000 * 1000)

void net_conn_init(NetConn *c, volatile bool *stop, volatile bool *cancel, volatile u32 *result)
{
    LightLock_Init(&c->lock);
    c->open = false;
    c->stop = stop;
    c->cancel = cancel;
    c->result = result;
}

static bool stopped(const NetConn *c) { return (c->stop && *c->stop) || (c->cancel && *c->cancel); }

bool net_get(NetConn *c, const char *path, u32 from, NetSink sink, void *ctx, u32 *status, const char *hdr, char *hdrBuf,
             size_t hdrSize)
{
    *status = 0;
    char url[160], range[32];
    snprintf(url, sizeof(url), "%s%s", STATS_URL, path);
    LightLock_Lock(&c->lock);
    Result r = stopped(c) ? -1 : httpcOpenContext(&c->ctx, HTTPC_METHOD_GET, url, 1);
    if (R_FAILED(r)) {
        LightLock_Unlock(&c->lock);
        *c->result = (u32)r;
        return false;
    }
    c->open = true;
    LightLock_Unlock(&c->lock);
    httpcAddRequestHeaderField(&c->ctx, "User-Agent", "3ds-recorder/" APP_VERSION);
    if (from) {
        snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)from);
        httpcAddRequestHeaderField(&c->ctx, "Range", range);
    }
    bool ok = false;
    u32 st = 0;
    if (R_SUCCEEDED(r = httpcBeginRequest(&c->ctx)) && R_SUCCEEDED(r = httpcGetResponseStatusCodeTimeout(&c->ctx, &st, CONNECT_NS)) &&
        st >= 100 && st <= 599) {
        *status = st;
        if ((st == 200 && !from) || (st == 206 && from)) {
            if (hdr && R_FAILED(httpcGetResponseHeader(&c->ctx, hdr, hdrBuf, hdrSize)))
                hdrBuf[0] = 0;
            u8 *buf = malloc(CHUNK);
            ok = buf != NULL;
            for (r = HTTPC_RESULTCODE_DOWNLOADPENDING; ok && r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING;) {
                if (stopped(c)) {
                    ok = false;
                    break;
                }
                u32 before = 0, after = 0;
                httpcGetDownloadSizeState(&c->ctx, &before, NULL);
                r = httpcReceiveDataTimeout(&c->ctx, buf, CHUNK, READ_NS);
                httpcGetDownloadSizeState(&c->ctx, &after, NULL);
                if (r != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING && R_FAILED(r)) {
                    ok = false;
                    break;
                }
                if (after > before && !sink(buf, after - before, ctx))
                    ok = false;
            }
            free(buf);
        }
    }
    if (R_FAILED(r))
        *c->result = (u32)r;
    LightLock_Lock(&c->lock);
    c->open = false;
    httpcCloseContext(&c->ctx);
    LightLock_Unlock(&c->lock);
    return ok;
}

void net_abort(NetConn *c)
{
    LightLock_Lock(&c->lock);
    if (c->open)
        httpcCancelConnection(&c->ctx);
    LightLock_Unlock(&c->lock);
}
