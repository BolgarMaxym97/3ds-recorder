/* The key in the "To phone" address (app/source/srvtoken.h). */
#include "srvtoken.h"

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

int main(void)
{
    const unsigned char bytes[SRV_TOKEN_BYTES] = {0x00, 0x1f, 0xa0, 0xff, 0x12, 0x34, 0xab, 0xcd};
    char t[SRV_TOKEN_LEN + 1];
    srv_token_encode(bytes, t);
    CHECK(strcmp(t, "001fa0ff1234abcd") == 0);

    CHECK(srv_token_ok("k=001fa0ff1234abcd", t));
    CHECK(srv_token_ok("f=20261123_101010.r3d&k=001fa0ff1234abcd", t));
    CHECK(srv_token_ok("m=page%20line&k=001fa0ff1234abcd", t));
    CHECK(srv_token_ok("k=001fa0ff1234abcd&m=x", t));
    CHECK(srv_token_ok("k=bad&k=001fa0ff1234abcd", t));      /* the second one */

    CHECK(!srv_token_ok(NULL, t));
    CHECK(!srv_token_ok("", t));
    CHECK(!srv_token_ok("k=", t));
    CHECK(!srv_token_ok("k=001fa0ff1234abce", t));            /* one character off */
    CHECK(!srv_token_ok("k=001fa0ff1234abc", t));             /* shorter */
    CHECK(!srv_token_ok("k=001fa0ff1234abcd0", t));           /* longer */
    CHECK(!srv_token_ok("kk=001fa0ff1234abcd", t));
    CHECK(!srv_token_ok("m=k=001fa0ff1234abcd", t));          /* inside another parameter */
    CHECK(!srv_token_ok("f=x", t));
    CHECK(!srv_token_ok("k=001fa0ff1234abcd", ""));           /* no key made: nothing passes */
    CHECK(!srv_token_ok("k=", ""));

    if (g_fail)
        return 1;
    printf("test_srvtoken: ok\n");
    return 0;
}
