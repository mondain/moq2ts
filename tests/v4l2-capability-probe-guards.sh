#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CAPS_HDR="$REPO_ROOT/src/media/V4l2Capabilities.h"
CAPS_CPP="$REPO_ROOT/src/media/V4l2Capabilities.cpp"
CAPTURE="$REPO_ROOT/src/media/LibavCaptureSource.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

grep -q 'V4l2Selection selectBestMode' "$CAPS_HDR" \
  || fail "V4l2Capabilities must declare selectBestMode"

grep -q 'kV4l2PixFmtMjpeg' "$CAPS_CPP" \
  || fail "selectBestMode must consider the MJPEG pixel format"

grep -q 'VIDIOC_ENUM_FRAMEINTERVALS' "$CAPS_CPP" \
  || fail "queryModes must enumerate frame intervals via ioctl"

grep -q 'groupNodesForCamera' "$CAPS_CPP" \
  || fail "V4l2Capabilities must group nodes per camera"

# The index==0 primary-device filter must be gone.
grep -q 'isPrimaryV4l2Device' "$CAPTURE" \
  && fail "isPrimaryV4l2Device filter must be removed from enumeration" || true

grep -q 'input_format' "$CAPTURE" \
  || fail "addVideo must request an input_format (mjpeg) when selected"

grep -q 'av_q2d(negRate)' "$CAPTURE" \
  || fail "addVideo must reconcile the encoder rate from the negotiated input rate"

# The probe path must be Linux-guarded.
grep -q '#if defined(__linux__)' "$CAPS_CPP" \
  || fail "queryModes/groupNodesForCamera must be guarded for Linux"

PREVIEW="$REPO_ROOT/src/media/LibavPreviewWorker.cpp"

grep -q 'CaptureOpen resolveCaptureOpen' "$CAPS_HDR" \
  || fail "V4l2Capabilities must declare resolveCaptureOpen"

grep -q 'resolveCaptureOpen' "$PREVIEW" \
  || fail "preview must select node/format via resolveCaptureOpen"

grep -q 'input_format' "$PREVIEW" \
  || fail "preview must request the MJPEG input_format when selected"

printf 'v4l2-capability-probe guards passed\n'
