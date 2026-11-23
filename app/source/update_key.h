/*
 * The public half of the key that signs the in-app update's manifest
 * (stats/scripts/update-manifest.mjs keygen; the private half stays on the maintainer's Mac).
 * Not a secret. A test build takes its own: make UPDATE_KEY=<header with UPDATE_PUBLIC_KEY>.
 */
#ifndef UPDATE_KEY_H
#define UPDATE_KEY_H

#ifdef UPDATE_KEY_FILE
#include UPDATE_KEY_FILE
#else
#define UPDATE_PUBLIC_KEY                                \
    {                                                    \
        0x8b, 0x21, 0x8a, 0x38, 0x2c, 0x24, 0x54, 0x8f,  \
        0xc9, 0xb7, 0x89, 0x1a, 0x65, 0x92, 0x98, 0xb1,  \
        0x3d, 0x21, 0x07, 0x01, 0x67, 0x2a, 0xb6, 0xaf,  \
        0xeb, 0xd1, 0xc5, 0x97, 0x74, 0xba, 0x78, 0x43,  \
    }
#endif

#endif
