#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CAPTURE="$REPO_ROOT/src/media/LibavCaptureSource.cpp"
HDR="$REPO_ROOT/src/media/LibavCaptureSource.h"
MUXER="$REPO_ROOT/src/media/MsftsMuxer.cpp"
M2TS_HDR="$REPO_ROOT/src/media/M2tsPacketizer.h"

fail() { printf '%s\n' "$1" >&2; exit 1; }

grep -q 'bool startsGroup' "$M2TS_HDR" \
  || fail "M2tsObject must carry a startsGroup flag"

grep -q 'AV_PKT_FLAG_KEY' "$CAPTURE" \
  || fail "capture must detect keyframes via AV_PKT_FLAG_KEY to mark group boundaries"

grep -q 'groupBoundaries' "$CAPTURE" \
  || fail "capture must track keyframe group boundaries"

grep -q 'object->startsGroup = true;' "$CAPTURE" \
  || fail "readObject must mark the first object of each group"

grep -q 'randomAccessActive' "$CAPTURE" \
  || fail "capture must expose randomAccessActive()"

grep -q 'randomAccessActive' "$HDR" \
  || fail "LibavCaptureSource must declare randomAccessActive()"

# m2tsRandomAccess must only be emitted behind the randomAccess flag.
grep -q 'if (catalog.randomAccess)' "$MUXER" \
  || fail "m2tsRandomAccess must be gated on catalog.randomAccess"

printf 'keyframe-aligned-groups guards passed\n'
