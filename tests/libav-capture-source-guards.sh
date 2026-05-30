#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE="$REPO_ROOT/src/media/LibavCaptureSource.cpp"

av_error_line="$(grep -n 'QString avError(int code)' "$SOURCE" | cut -d: -f1)"
guard_line="$(grep -n '^#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE$' "$SOURCE" | sed -n '2p' | cut -d: -f1)"

if [[ -z "$av_error_line" || -z "$guard_line" ]]; then
  printf 'expected avError and the libav capture implementation guard to exist\n' >&2
  exit 1
fi

if (( av_error_line < guard_line )); then
  printf 'avError should live inside the libav capture implementation guard\n' >&2
  exit 1
fi
