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

if grep -q 'd.id = QString::number(i);' "$SOURCE"; then
  printf 'libav capture enumeration should keep AVDeviceInfo device_name as the openable device id\n' >&2
  exit 1
fi

if ! grep -q 'info->device_name ? info->device_name' "$SOURCE"; then
  printf 'libav capture enumeration does not preserve AVDeviceInfo device_name\n' >&2
  exit 1
fi

if ! grep -q 'if (!info->device_name || !\*info->device_name) continue;' "$SOURCE"; then
  printf 'libav capture enumeration should skip devices without an openable device_name\n' >&2
  exit 1
fi

if ! grep -q '/sys/class/video4linux' "$SOURCE"; then
  printf 'libav capture enumeration should filter duplicate V4L2 companion devices by sysfs index\n' >&2
  exit 1
fi

if ! grep -q 'isPrimaryV4l2Device' "$SOURCE"; then
  printf 'libav capture enumeration should use a V4L2 primary-device helper\n' >&2
  exit 1
fi

if grep -q 'MPEG-TS capture muxer did not emit PAT/PMT initData' "$SOURCE"; then
  printf 'live libav capture should not fail startup when PAT/PMT initData is delayed\n' >&2
  exit 1
fi

if ! grep -q 'extractInitData(muxedBytes, &initDataBytes, &pmtPidValue, &pcrPidValue);' "$SOURCE"; then
  printf 'live libav capture should attempt initData extraction without requiring it\n' >&2
  exit 1
fi

if ! grep -q 'AVAudioFifo' "$SOURCE"; then
  printf 'live libav capture should buffer audio into encoder-sized frames\n' >&2
  exit 1
fi

if ! grep -q 'encoder->frame_size' "$SOURCE"; then
  printf 'live libav capture should honor encoder frame_size for audio frames\n' >&2
  exit 1
fi

if ! grep -q 'sendEncoderFrame(stream, frame.get(), "audio", error)' "$SOURCE"; then
  printf 'live libav capture errors should identify audio frame encoder failures\n' >&2
  exit 1
fi
