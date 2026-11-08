/*
 * CTRPluginFramework headers include "types.h" (CTRPF's own copy of libctru's
 * types plus a few attribute macros). The libctrpf package does not ship it,
 * so this shim provides it on top of current libctru.
 */
#pragma once
#ifndef BENCH_CTRPF_TYPES_H
#define BENCH_CTRPF_TYPES_H

#include <3ds/types.h>

#ifndef NORETURN
#define NORETURN __attribute__((noreturn))
#endif
#ifndef PACKED
#define PACKED __attribute__((packed))
#endif
#ifndef USED
#define USED __attribute__((used))
#endif
#ifndef UNUSED
#define UNUSED __attribute__((unused))
#endif
#ifndef NAKED
#define NAKED __attribute__((naked))
#endif
#ifndef DEPRECATED
#define DEPRECATED __attribute__((deprecated))
#endif
#ifndef ALIGN
#define ALIGN(m) __attribute__((aligned(m)))
#endif

#endif
