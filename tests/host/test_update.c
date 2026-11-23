/* The in-app update's manifest, versions, notes and signature (app/source/update_core.c). The
 * manifest and signature below were made by stats/scripts/update-manifest.mjs with a throwaway key:
 * Node's Ed25519 against monocypher's. */
#include "update_core.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

static const char MANIFEST[] =
    "version=1.6.1\n"
    "cia=1000 a76199da833aded1da0c5d186d15c4ed3bee5d1f8fda5e485435d913eb8097b7\n"
    "3dsx=500 390197abd8aeff69d2c80326ce9f3e2cf5bbbf75ea3647320acd2ee339d5fe02\n"
    "notes.en=Buttons in recordings|Faster start\n"
    "notes.ja=録画にボタン表示\n"
    "notes.uk=Кнопки в записах\n";
static const char SIG[] = "8e80637305972034a93a306faece4d97e75cfe0b6803b2a7bd617e24b0d7a668"
                          "c4606d86d751bbe41b4cbd24aeebd9e245400d0560ef585ae3326c1a829dff08";
static const uint8_t KEY[32] = {
    0x60, 0xb3, 0x91, 0x08, 0xf8, 0x4b, 0x2b, 0xf2, 0xa7, 0x73, 0x76, 0x4b, 0x03, 0xa5, 0x8d, 0xbb,
    0x15, 0xe2, 0x94, 0x4f, 0x17, 0x42, 0xd1, 0xbb, 0x91, 0xd0, 0xbb, 0xf4, 0x4b, 0x8f, 0x4f, 0x66,
};

int main(void)
{
    const size_t len = sizeof(MANIFEST) - 1;
    UpdManifest m;

    /* parse */
    CHECK(upd_parse(MANIFEST, len, &m));
    CHECK(strcmp(m.version, "1.6.1") == 0);
    CHECK(m.cia.size == 1000 && m.x3dsx.size == 500);
    CHECK(strcmp(m.cia.sha256, "a76199da833aded1da0c5d186d15c4ed3bee5d1f8fda5e485435d913eb8097b7") == 0);
    CHECK(strcmp(m.x3dsx.sha256, "390197abd8aeff69d2c80326ce9f3e2cf5bbbf75ea3647320acd2ee339d5fe02") == 0);
    CHECK(!upd_parse("", 0, &m));
    CHECK(!upd_parse("version=1.6\n", 12, &m));
    const char crlf[] = "version=2.0.0\r\ncia=5 " "0000000000000000000000000000000000000000000000000000000000000000\r\n"
                        "3dsx=6 0000000000000000000000000000000000000000000000000000000000000000\r\n";
    CHECK(upd_parse(crlf, sizeof(crlf) - 1, &m) && strcmp(m.version, "2.0.0") == 0 && m.x3dsx.size == 6);
    const char upper[] = "version=2.0.0\ncia=5 ABCDEF0000000000000000000000000000000000000000000000000000000000\n"
                         "3dsx=6 0000000000000000000000000000000000000000000000000000000000000000\n";
    CHECK(!upd_parse(upper, sizeof(upper) - 1, &m));
    const char nosize[] = "version=2.0.0\ncia=0 0000000000000000000000000000000000000000000000000000000000000000\n"
                          "3dsx=6 0000000000000000000000000000000000000000000000000000000000000000\n";
    CHECK(!upd_parse(nosize, sizeof(nosize) - 1, &m));
    const char no3dsx[] = "version=2.0.0\ncia=5 0000000000000000000000000000000000000000000000000000000000000000\n";
    CHECK(!upd_parse(no3dsx, sizeof(no3dsx) - 1, &m));
    const char badver[] = "version=2.x.0\ncia=5 0000000000000000000000000000000000000000000000000000000000000000\n"
                          "3dsx=6 0000000000000000000000000000000000000000000000000000000000000000\n";
    CHECK(!upd_parse(badver, sizeof(badver) - 1, &m));

    /* versions */
    CHECK(upd_version_cmp("1.10.0", "1.9.2") > 0);
    CHECK(upd_version_cmp("1.6.0", "1.6.0") == 0);
    CHECK(upd_version_cmp("1.6.0", "1.6.1") < 0);
    CHECK(upd_version_cmp("2.0.0", "1.99.99") > 0);
    CHECK(upd_version_cmp("1.6", "1.6.0") == 0);

    /* notes */
    char lines[UPD_NOTES_MAX][UPD_NOTE_LEN];
    CHECK(upd_notes(MANIFEST, len, "en", lines, UPD_NOTES_MAX) == 2);
    CHECK(strcmp(lines[0], "Buttons in recordings") == 0 && strcmp(lines[1], "Faster start") == 0);
    CHECK(upd_notes(MANIFEST, len, "uk", lines, UPD_NOTES_MAX) == 1 && strcmp(lines[0], "Кнопки в записах") == 0);
    CHECK(upd_notes(MANIFEST, len, "ja", lines, UPD_NOTES_MAX) == 1 && strcmp(lines[0], "録画にボタン表示") == 0);
    CHECK(upd_notes(MANIFEST, len, "de", lines, UPD_NOTES_MAX) == 2);            /* English instead */
    CHECK(strcmp(lines[0], "Buttons in recordings") == 0);
    CHECK(upd_notes(MANIFEST, len, "e", lines, UPD_NOTES_MAX) == 2);             /* not a prefix match */
    CHECK(upd_notes(MANIFEST, len, "en", lines, 1) == 1);
    CHECK(upd_notes("version=1.0.0\n", 14, "en", lines, UPD_NOTES_MAX) == 0);
    const char empty[] = "notes.en=a||b|\n";
    CHECK(upd_notes(empty, sizeof(empty) - 1, "en", lines, UPD_NOTES_MAX) == 2 && !strcmp(lines[1], "b"));
    /* a long Ukrainian line is cut between characters, never inside one */
    char longer[1200] = "notes.uk=";
    for (int i = 0; i < 250; i++)
        strcat(longer, "ї");
    CHECK(upd_notes(longer, strlen(longer), "uk", lines, UPD_NOTES_MAX) == 1);
    CHECK(strlen(lines[0]) == UPD_NOTE_LEN - 1 - (UPD_NOTE_LEN - 1) % 2 && strlen(lines[0]) % 2 == 0);

    /* signature */
    CHECK(upd_sig_ok(MANIFEST, len, SIG, KEY));
    char changed[sizeof(MANIFEST)];
    memcpy(changed, MANIFEST, sizeof(MANIFEST));
    changed[12] = '2';                                                 /* version=1.6.2 */
    CHECK(!upd_sig_ok(changed, len, SIG, KEY));
    CHECK(!upd_sig_ok(MANIFEST, len - 1, SIG, KEY));
    char sig[129];
    memcpy(sig, SIG, sizeof(sig));
    sig[0] = sig[0] == '8' ? '9' : '8';
    CHECK(!upd_sig_ok(MANIFEST, len, sig, KEY));
    CHECK(!upd_sig_ok(MANIFEST, len, "", KEY));
    CHECK(!upd_sig_ok(MANIFEST, len, NULL, KEY));
    uint8_t other[32];
    memcpy(other, KEY, 32);
    other[31] ^= 1;
    CHECK(!upd_sig_ok(MANIFEST, len, SIG, other));

    /* sha256 + hex: the file's digest as the manifest writes it */
    uint8_t d[32];
    char hex[65];
    sha256("abc", 3, d);
    upd_hex(d, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    if (g_fail)
        return 1;
    printf("test_update: ok\n");
    return 0;
}
