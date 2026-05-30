#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="moq2ts-bookworm-builder"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CONTAINER_BUILD_DIR="build-bookworm"

printf 'Building Docker image: %s\n' "$IMAGE_NAME"
docker build -t "$IMAGE_NAME" -f "$REPO_ROOT/docker/Dockerfile.debian-bookworm" "$REPO_ROOT"

printf 'Compiling project in Bookworm container...\n'
docker run --rm -t \
  --user "$(id -u):$(id -g)" \
  --env HOME=/tmp \
  --env XDG_CACHE_HOME=/tmp \
  -v "$REPO_ROOT:/workspace/moq2ts:rw" \
  -w /workspace/moq2ts \
  "$IMAGE_NAME" bash -lc '
  mkdir -p '"$CONTAINER_BUILD_DIR"' && \
  cmake -S . -B '"$CONTAINER_BUILD_DIR"' \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-Wall" \
    -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=ON && \
  cmake --build '"$CONTAINER_BUILD_DIR"' -j"$(nproc)" && \
  cmake --install '"$CONTAINER_BUILD_DIR"' --prefix /workspace/moq2ts/'"$CONTAINER_BUILD_DIR"'/install && \
  bash /workspace/moq2ts/scripts/bundle-bookworm-runtime.sh /workspace/moq2ts/'"$CONTAINER_BUILD_DIR"'/install
'

printf 'Build complete. Launcher: %s\n' "$REPO_ROOT/$CONTAINER_BUILD_DIR/install/moq2ts-publisher-bookworm"
