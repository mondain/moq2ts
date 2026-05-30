#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_SCRIPT="$REPO_ROOT/scripts/build-debian-bookworm.sh"
DOCKERFILE="$REPO_ROOT/docker/Dockerfile.debian-bookworm"

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

if ! grep -q 'bundle-bookworm-runtime.sh' "$BUILD_SCRIPT"; then
  printf 'Bookworm build script does not bundle runtime libraries\n' >&2
  exit 1
fi

if ! grep -q -- '--user "$(id -u):$(id -g)"' "$BUILD_SCRIPT"; then
  printf 'Bookworm build script does not run the compile container as the invoking user\n' >&2
  exit 1
fi

if grep -q 'chown -R' "$BUILD_SCRIPT"; then
  printf 'Bookworm build script still relies on chown after creating root-owned files\n' >&2
  exit 1
fi

for package in libavdevice-dev libswscale-dev patchelf; do
  if ! grep -q "$package" "$DOCKERFILE"; then
    printf 'Bookworm Docker image is missing %s\n' "$package" >&2
    exit 1
  fi
done

if grep -q 'qtmultimedia5-dev' "$DOCKERFILE"; then
  printf 'Bookworm Docker image still installs unused Qt Multimedia development package\n' >&2
  exit 1
fi

if ! grep -q 'MOQ2TS_HAVE_LIBAV_CAPTURE' "$REPO_ROOT/src/app/MainWindow.cpp"; then
  printf 'MainWindow does not use libav capture enumeration when it is available\n' >&2
  exit 1
fi

if ! grep -q 'qt.conf' "$REPO_ROOT/scripts/bundle-bookworm-runtime.sh"; then
  printf 'Bookworm runtime bundler does not create qt.conf\n' >&2
  exit 1
fi

if ! grep -q 'patchelf --set-rpath' "$REPO_ROOT/scripts/bundle-bookworm-runtime.sh"; then
  printf 'Bookworm runtime bundler does not set relative rpaths\n' >&2
  exit 1
fi

if ! grep -q 'MOQ2TS_QT_PLATFORM_PLUGINS' "$REPO_ROOT/scripts/bundle-bookworm-runtime.sh"; then
  printf 'Bookworm runtime bundler does not expose a Qt platform plugin allowlist\n' >&2
  exit 1
fi

if grep -q 'cp -a "$qt_plugin_root/platforms"' "$REPO_ROOT/scripts/bundle-bookworm-runtime.sh"; then
  printf 'Bookworm runtime bundler still copies every Qt platform plugin\n' >&2
  exit 1
fi

for plugin_dir in xcbglintegrations imageformats; do
  if grep -q "$plugin_dir" "$REPO_ROOT/scripts/bundle-bookworm-runtime.sh"; then
    printf 'Bookworm runtime bundler still copies unused Qt plugin directory %s\n' "$plugin_dir" >&2
    exit 1
  fi
done
