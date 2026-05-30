#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_SCRIPT="$REPO_ROOT/scripts/build-debian-bookworm.sh"

if ! grep -q 'CONTAINER_BUILD_DIR="build-bookworm"' "$BUILD_SCRIPT"; then
  printf 'expected Bookworm build script to use an isolated container build directory\n' >&2
  exit 1
fi

if grep -q -- '-B build' "$BUILD_SCRIPT"; then
  printf 'Bookworm build script still configures CMake into shared build/\n' >&2
  exit 1
fi

if grep -q '/workspace/moq2ts/build/install' "$BUILD_SCRIPT"; then
  printf 'Bookworm build script still installs into shared build/install\n' >&2
  exit 1
fi
