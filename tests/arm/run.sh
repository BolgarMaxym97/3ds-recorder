#!/usr/bin/env bash
# The ARM assembly against its C reference, on an ARM CPU (Docker linux/arm/v7, emulated on a Mac).
# Usage: tests/arm/run.sh [recording.r3d ...]   (paths inside the repo)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
exec docker run --rm --platform linux/arm/v7 -v "$REPO:/work" -w /work arm32v7/gcc:13 sh -c \
    "gcc -O2 -marm -march=armv6k+fp -mfpu=vfpv2 -Wall -Wextra -Icodec/include -o /tmp/t tests/arm/test_q565_arm.c codec/source/codec.c codec/source/q565_arm.s && /tmp/t $*"
