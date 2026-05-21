#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="moq2ts-bookworm-builder"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

printf 'Building Docker image: %s\n' "$IMAGE_NAME"
docker build -t "$IMAGE_NAME" -f "$REPO_ROOT/docker/Dockerfile.debian-bookworm" "$REPO_ROOT"

printf 'Compiling project in Bookworm container...\n'
docker run --rm -t -v "$REPO_ROOT:/workspace/moq2ts:rw" -w /workspace/moq2ts "$IMAGE_NAME" bash -lc '
  mkdir -p build && \
  cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-Wall" \
    -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=ON && \
  cmake --build build -j"$(nproc)" && \
  cmake --install build --prefix /workspace/moq2ts/build/install
'

printf 'Build complete. Binary: %s\n' "$REPO_ROOT/build/install/bin/moq2ts-publisher"
