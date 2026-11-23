/*
 * "To phone": the key in the QR code's address (?f=<name>&k=<key>). Without it anyone on the same
 * Wi-Fi could read the recording and /log.txt (console model, Luma version, logs) while the
 * transfer screen is open. The page is served without the key (it is the same for everyone); the
 * data (/api/..., /rec/..., /log.txt) only with it. Pure C, tested on the host (tests/host/test_srvtoken).
 */
#ifndef SRVTOKEN_H
#define SRVTOKEN_H

#include <stdbool.h>
#include <stddef.h>

#define SRV_TOKEN_BYTES 8
#define SRV_TOKEN_LEN (SRV_TOKEN_BYTES * 2)   /* lower-case hex */

static inline void srv_token_encode(const unsigned char bytes[SRV_TOKEN_BYTES], char out[SRV_TOKEN_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < SRV_TOKEN_BYTES; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 15];
    }
    out[SRV_TOKEN_LEN] = 0;
}

/* query: what follows '?' (NULL: none). True when one of its parameters is k=<token> exactly. */
static inline bool srv_token_ok(const char *query, const char *token)
{
    if (!query || !token || !token[0])
        return false;
    for (const char *p = query; *p;) {
        if (p[0] == 'k' && p[1] == '=') {
            const char *v = p + 2;
            size_t n = 0;
            unsigned diff = 0;
            while (v[n] && v[n] != '&' && n < SRV_TOKEN_LEN + 1)
                n++;
            if (n == SRV_TOKEN_LEN && (v[n] == 0 || v[n] == '&')) {
                for (size_t i = 0; i < n; i++)   /* every character compared: no early way out */
                    diff |= (unsigned)(v[i] ^ token[i]);
                if (!diff && !token[n])
                    return true;
            }
        }
        while (*p && *p != '&')
            p++;
        if (*p == '&')
            p++;
    }
    return false;
}

#endif
