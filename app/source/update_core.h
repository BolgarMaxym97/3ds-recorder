/*
 * In-app update, the part without the console: the signed manifest the Worker serves
 * (stats/src/update.ts, written by stats/scripts/update-manifest.mjs), versions, the notes.
 * Pure C, tested on the host (tests/host/test_update).
 *
 *   version=1.7.0
 *   cia=<bytes> <sha256 hex>
 *   3dsx=<bytes> <sha256 hex>
 *   notes.<lang>=<line>|<line>...
 */
#ifndef UPDATE_CORE_H
#define UPDATE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPD_MANIFEST_MAX 8192   /* bytes; a bigger answer is not ours */
#define UPD_NOTES_MAX 6         /* lines */
#define UPD_NOTE_LEN 384        /* bytes of one line (120 characters, 3 bytes each in Japanese) */

typedef struct {
    uint32_t size;
    char sha256[65];            /* lower-case hex */
} UpdFile;

typedef struct {
    char version[16];
    UpdFile cia, x3dsx;
} UpdManifest;

/* false when a line is missing or malformed: the manifest is then not used. */
bool upd_parse(const char *text, size_t len, UpdManifest *m);

/* "1.10.0" vs "1.9.2": <0, 0, >0. A part that is not a number counts as 0. */
int upd_version_cmp(const char *a, const char *b);

/* The notes for `lang` ("uk", "zh-Hans"; English when that language has none), split at '|' into
 * lines (cut to UPD_NOTE_LEN - 1 bytes at a character boundary). Returns the line count. */
int upd_notes(const char *text, size_t len, const char *lang, char lines[][UPD_NOTE_LEN], int max);

/* The Ed25519 signature (128 hex characters, X-Sig) of exactly `len` bytes of `body`. */
bool upd_sig_ok(const void *body, size_t len, const char *sig_hex, const uint8_t public_key[32]);

void upd_hex(const uint8_t digest[32], char out[65]);

#endif
