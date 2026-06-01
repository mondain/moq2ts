#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE="$REPO_ROOT/src/media/LibavCaptureSource.cpp"
PIPELINE="$REPO_ROOT/src/media/LivePipeline.cpp"

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

if ! grep -q 'groupNodesForCamera' "$SOURCE"; then
  printf 'libav capture enumeration should group V4L2 nodes per physical camera\n' >&2
  exit 1
fi

if ! grep -q 'candidateNodes' "$SOURCE"; then
  printf 'libav capture enumeration should record per-camera candidate nodes\n' >&2
  exit 1
fi

CAPS="$REPO_ROOT/src/media/V4l2Capabilities.cpp"
if ! grep -q '/sys/class/video4linux' "$CAPS"; then
  printf 'V4L2 node grouping should resolve cameras via sysfs\n' >&2
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

# A corrupt input frame (e.g. transient MJPEG garbage at camera startup) must
# be skipped, not abort the capture session.
if ! grep -q 'AVERROR_INVALIDDATA || rc == AVERROR(EINVAL)' "$SOURCE"; then
  printf 'live libav capture should skip corrupt decode frames instead of aborting\n' >&2
  exit 1
fi

# Full-range MJPEG (yuvj*) frames must have the swscale color range set so the
# conversion to the encoder's limited-range YUV420P is correct (and quiet).
if ! grep -q 'applyScalerRange(' "$SOURCE"; then
  printf 'live libav capture should set swscale color range for full-range frames\n' >&2
  exit 1
fi

# The decode->encode->mux pipeline should log a one-shot confirmation at open.
if ! grep -q 'decode=%s -> encode=%s -> mux=%s' "$SOURCE"; then
  printf 'live libav capture should log the decode->encode->mux pipeline at open\n' >&2
  exit 1
fi

# Audio and video PTS must share one capture epoch (A/V timeline).
if ! grep -q 'captureEpochUs' "$SOURCE"; then
  printf 'capture must stamp PTS from a shared capture epoch\n' >&2
  exit 1
fi
if ! grep -q 'ensureCaptureEpoch' "$SOURCE"; then
  printf 'capture must set the shared epoch once via ensureCaptureEpoch\n' >&2
  exit 1
fi

# Audio PTS must be epoch-anchored, not a bare from-zero counter.
if ! grep -q 'audioAnchored' "$SOURCE"; then
  printf 'audio PTS must anchor to the shared capture epoch\n' >&2
  exit 1
fi

# Objects must carry a real media time for pacing (offset->mediaUs map).
if ! grep -q 'offsetMediaUs' "$SOURCE"; then
  printf 'capture must record video media time per byte offset for pacing\n' >&2
  exit 1
fi
if ! grep -q 'object->mediaTimeUs = lastVideoMediaUs' "$SOURCE"; then
  printf 'readObject must tag objects with the real video media time\n' >&2
  exit 1
fi

# The pacing wait must be stop-checkable (bounded), not an unbounded sleep.
if ! grep -q 'paceDelayUs(' "$PIPELINE"; then
  printf 'pipeline must pace object release via paceDelayUs\n' >&2
  exit 1
fi
if ! grep -q 'm_running.load' "$PIPELINE"; then
  printf 'pipeline pacing wait must re-check m_running each step\n' >&2
  exit 1
fi

printf 'libav-capture-source guards passed\n'
