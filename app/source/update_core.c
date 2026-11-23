#include "update_core.h"

#include <stdlib.h>
#include <string.h>

#include "monocypher-ed25519.h"

/* The value of the line "<key>=..." (key without '='), its length in *vlen; NULL when absent. */
static const char *find(const char *text, size_t len, const char *key, size_t *vlen)
{
    const size_t klen = strlen(key);
    for (size_t at = 0; at < len;) {
        const char *line = text + at;
        const char *nl = memchr(line, '\n', len - at);
        const size_t n = nl ? (size_t)(nl - line) : len - at;
        if (n > klen && !memcmp(line, key, klen) && line[klen] == '=') {
            *vlen = n - klen - 1;
            if (*vlen && line[n - 1] == '\r')
                (*vlen)--;
            return line + klen + 1;
        }
        at += n + 1;
    }
    return NULL;
}

static bool is_version(const char *v, size_t n)
{
    int dots = 0;
    bool digit = false;
    for (size_t i = 0; i < n; i++) {
        if (v[i] == '.') {
            if (!digit)
                return false;
            dots++;
            digit = false;
        } else if (v[i] >= '0' && v[i] <= '9') {
            digit = true;
        } else {
            return false;
        }
    }
    return dots == 2 && digit;
}

static bool parse_file(const char *text, size_t len, const char *key, UpdFile *f)
{
    size_t n;
    const char *v = find(text, len, key, &n);
    if (!v || n < 66 || n > 76)
        return false;
    char buf[80];
    memcpy(buf, v, n);
    buf[n] = 0;
    char *end;
    const unsigned long size = strtoul(buf, &end, 10);
    if (end == buf || *end != ' ' || size == 0 || size > 0x7FFFFFFFul || strlen(end + 1) != 64)
        return false;
    for (int i = 0; i < 64; i++) {
        const char c = end[1 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    f->size = (uint32_t)size;
    memcpy(f->sha256, end + 1, 65);
    return true;
}

bool upd_parse(const char *text, size_t len, UpdManifest *m)
{
    memset(m, 0, sizeof(*m));
    size_t n;
    const char *v = find(text, len, "version", &n);
    if (!v || n >= sizeof(m->version) || !is_version(v, n))
        return false;
    memcpy(m->version, v, n);
    return parse_file(text, len, "cia", &m->cia) && parse_file(text, len, "3dsx", &m->x3dsx);
}

static unsigned part(const char **s)
{
    unsigned n = 0;
    while (**s >= '0' && **s <= '9')
        n = n * 10 + (unsigned)(*(*s)++ - '0');
    while (**s && **s != '.')
        (*s)++;
    if (**s == '.')
        (*s)++;
    return n;
}

int upd_version_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        const unsigned x = part(&a), y = part(&b);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

/* cut at a UTF-8 character boundary */
static size_t fit(const char *s, size_t n, size_t cap)
{
    if (n < cap)
        return n;
    n = cap - 1;
    while (n && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

int upd_notes(const char *text, size_t len, const char *lang, char lines[][UPD_NOTE_LEN], int max)
{
    char key[24] = "notes.";
    strncat(key, lang ? lang : "en", sizeof(key) - 7);
    size_t n;
    const char *v = find(text, len, key, &n);
    if (!v)
        v = find(text, len, "notes.en", &n);
    if (!v)
        return 0;
    int count = 0;
    for (size_t at = 0; at <= n && count < max;) {
        const char *bar = memchr(v + at, '|', n - at);
        const size_t one = bar ? (size_t)(bar - (v + at)) : n - at;
        if (one) {
            const size_t k = fit(v + at, one, UPD_NOTE_LEN);
            memcpy(lines[count], v + at, k);
            lines[count++][k] = 0;
        }
        if (!bar)
            break;
        at += one + 1;
    }
    return count;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool upd_sig_ok(const void *body, size_t len, const char *sig_hex, const uint8_t public_key[32])
{
    uint8_t sig[64];
    if (!sig_hex || strlen(sig_hex) != 128)
        return false;
    for (int i = 0; i < 64; i++) {
        const int hi = hex_digit(sig_hex[2 * i]), lo = hex_digit(sig_hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        sig[i] = (uint8_t)(hi << 4 | lo);
    }
    return crypto_ed25519_check(sig, public_key, body, len) == 0;
}

void upd_hex(const uint8_t digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
}
