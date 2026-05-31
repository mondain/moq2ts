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

grep -q 'scanForRapBoundaries' "$CAPTURE" \
  || fail "capture must scan muxed TS for random-access points"

grep -q 'random_access_indicator' "$CAPTURE" \
  || fail "RAP scan must test the TS random_access_indicator"

grep -q 'videoPidValue' "$CAPTURE" \
  || fail "capture must track the muxer-assigned video PID for RAP scanning"

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
