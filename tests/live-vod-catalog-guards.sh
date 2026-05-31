#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MUXER="$REPO_ROOT/src/media/MsftsMuxer.cpp"
HDR="$REPO_ROOT/src/media/MsftsMuxer.h"
PKT="$REPO_ROOT/src/media/M2tsPacketizer.cpp"
PIPE="$REPO_ROOT/src/media/LivePipeline.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

grep -q 'qint64 trackDurationMs' "$HDR" \
  || fail "MsftsCatalog must carry trackDurationMs"
grep -q 'bool randomAccess' "$HDR" \
  || fail "MsftsCatalog must carry randomAccess"

# trackDuration must be VOD-gated (never emitted when isLive is true).
grep -q '!catalog.isLive && catalog.trackDurationMs > 0' "$MUXER" \
  || fail "trackDuration must be gated on !isLive && trackDurationMs > 0"
grep -q 'if (catalog.randomAccess)' "$MUXER" \
  || fail "m2tsRandomAccess must be gated on catalog.randomAccess"

# Duration probe exists and is libav-guarded.
grep -q 'probeDurationMs' "$PKT" \
  || fail "M2tsPacketizer must implement probeDurationMs"
grep -q 'MOQ2TS_HAVE_LIBAV_CAPTURE' "$PKT" \
  || fail "probeDurationMs must be guarded by MOQ2TS_HAVE_LIBAV_CAPTURE"

# Pipeline marks file path VOD and capture path live.
grep -q '.randomAccess = true,' "$PIPE" \
  || fail "file path must advertise randomAccess for VOD"
grep -q '.trackDurationMs = M2tsPacketizer::probeDurationMs' "$PIPE" \
  || fail "file path must source trackDurationMs from the probe"
grep -q '.isLive = false,' "$PIPE" \
  || fail "file path must set isLive=false"
grep -q '.isLive = true,' "$PIPE" \
  || fail "capture path must set isLive=true"

printf 'live-vod-catalog guards passed\n'
