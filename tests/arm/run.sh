#!/usr/bin/env bash
# The ARM assembly against its C reference, on an ARM CPU (Docker linux/arm/v7, emulated on a Mac).
# Usage: tests/arm/run.sh [recording.r3d ...]   (paths inside the repo; the Q565 test reads them)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
exec docker run --rm --platform linux/arm/v7 -v "$REPO:/work" -w /work arm32v7/gcc:13 sh -c \
    "CF='-O2 -marm -march=armv6k+fp -mfpu=vfpv2 -Wall -Wextra -Icodec/include' && \
     gcc \$CF -o /tmp/q tests/arm/test_q565_arm.c codec/source/codec.c codec/source/q565_arm.s && \
     gcc \$CF -o /tmp/c tests/arm/test_c420_arm.c codec/source/codec.c codec/source/c420_arm.s && \
     gcc \$CF -o /tmp/v tests/arm/test_convert_arm.c codec/source/codec.c codec/source/convert.c codec/source/convert_arm.s && \
     /tmp/q $* && /tmp/c && /tmp/v"
