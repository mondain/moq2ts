#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN_WINDOW_CPP="$ROOT_DIR/src/app/MainWindow.cpp"
MAIN_WINDOW_H="$ROOT_DIR/src/app/MainWindow.h"
PREVIEW_PANEL_H="$ROOT_DIR/src/app/PreviewPanel.h"
PREVIEW_PANEL_CPP="$ROOT_DIR/src/app/PreviewPanel.cpp"
PREVIEW_WORKER_H="$ROOT_DIR/src/media/LibavPreviewWorker.h"
PREVIEW_WORKER_CPP="$ROOT_DIR/src/media/LibavPreviewWorker.cpp"
CAPTURE_SOURCE="$ROOT_DIR/src/media/LibavCaptureSource.cpp"
LIVE_PIPELINE_H="$ROOT_DIR/src/media/LivePipeline.h"
LIVE_PIPELINE_CPP="$ROOT_DIR/src/media/LivePipeline.cpp"
MAIN_CPP="$ROOT_DIR/src/main.cpp"
CMAKE_FILE="$ROOT_DIR/CMakeLists.txt"

for file in "$PREVIEW_PANEL_H" "$PREVIEW_PANEL_CPP" "$PREVIEW_WORKER_H" "$PREVIEW_WORKER_CPP"; do
  if [[ ! -f "$file" ]]; then
    printf 'missing preview file: %s\n' "${file#$ROOT_DIR/}" >&2
    exit 1
  fi
done

if ! grep -q 'QTabWidget' "$MAIN_WINDOW_H" "$MAIN_WINDOW_CPP"; then
  printf 'main window should use tabs for config, preview, and logs\n' >&2
  exit 1
fi

if ! grep -q 'PreviewPanel' "$MAIN_WINDOW_H" "$MAIN_WINDOW_CPP"; then
  printf 'main window should own and update a PreviewPanel\n' >&2
  exit 1
fi

if ! grep -q 'addTab(previewPanel' "$MAIN_WINDOW_CPP"; then
  printf 'preview panel should be available as a tab\n' >&2
  exit 1
fi

if ! grep -q 'videoFrameReady(const QImage&' "$PREVIEW_WORKER_H"; then
  printf 'preview worker should emit decoded video frames\n' >&2
  exit 1
fi

if ! grep -q 'audioLevelsChanged(double left' "$PREVIEW_WORKER_H"; then
  printf 'preview worker should emit stereo audio levels\n' >&2
  exit 1
fi

if ! grep -q 'av_read_frame' "$PREVIEW_WORKER_CPP"; then
  printf 'preview worker should read from libav capture devices\n' >&2
  exit 1
fi

if ! grep -q 'sws_scale' "$PREVIEW_WORKER_CPP"; then
  printf 'preview worker should convert decoded video frames to a display format\n' >&2
  exit 1
fi

if ! grep -q 'audioLevelsChanged' "$PREVIEW_PANEL_CPP"; then
  printf 'preview panel should update audio meters from worker levels\n' >&2
  exit 1
fi

if ! grep -q 'std::log10' "$PREVIEW_PANEL_CPP"; then
  printf 'preview audio meters should use dBFS scaling so normal speech is visible\n' >&2
  exit 1
fi

if grep -q 'level \\* 100.0' "$PREVIEW_PANEL_CPP"; then
  printf 'preview audio meters should not display raw linear RMS values\n' >&2
  exit 1
fi

if ! grep -q 'Qt::DirectConnection' "$PREVIEW_PANEL_CPP"; then
  printf 'preview stop should reach the capture worker even while its capture loop is active\n' >&2
  exit 1
fi

if ! grep -q 'QMetaObject::invokeMethod(m_worker' "$PREVIEW_PANEL_CPP"; then
  printf 'preview start should queue directly onto the worker object without an unregistered PublishConfig signal argument\n' >&2
  exit 1
fi

if grep -q 'emit startWorker' "$PREVIEW_PANEL_CPP"; then
  printf 'preview start should not emit PublishConfig across threads without metatype registration\n' >&2
  exit 1
fi

if ! grep -q 'setPreviewCallbacks' "$CAPTURE_SOURCE" "$LIVE_PIPELINE_CPP"; then
  printf 'publishing capture path should feed preview from the same decoded frames\n' >&2
  exit 1
fi

if ! grep -q 'previewVideoFrame' "$LIVE_PIPELINE_H" "$MAIN_CPP"; then
  printf 'live pipeline should expose video preview frames to the UI while publishing\n' >&2
  exit 1
fi

if ! grep -q 'previewAudioLevels' "$LIVE_PIPELINE_H" "$MAIN_CPP"; then
  printf 'live pipeline should expose audio levels to the UI while publishing\n' >&2
  exit 1
fi

for source in src/app/PreviewPanel.cpp src/media/LibavPreviewWorker.cpp; do
  if ! grep -q "$source" "$CMAKE_FILE"; then
    printf 'CMake is missing %s\n' "$source" >&2
    exit 1
  fi
done
