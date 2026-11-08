#!/usr/bin/env bash
# Run a command inside the dev Docker image with the repo mounted.
# Image: 3dsrec-dev (devkitARM + libctrpf + 3gxtool), build it once with
#   docker build -t 3dsrec-dev tools/
# Usage: tools/dkp.sh <dir relative to repo> <command...>
#   tools/dkp.sh tests/sdbench make
#   tools/dkp.sh tests/benchplugin make
set -euo pipefail

IMAGE="${DKP_IMAGE:-3dsrec-dev}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:?usage: tools/dkp.sh <dir> <command...>}"
shift

exec docker run --rm \
    -v "$REPO:/work" \
    -w "/work/$DIR" \
    --user "$(id -u):$(id -g)" \
    "$IMAGE" \
    "$@"
