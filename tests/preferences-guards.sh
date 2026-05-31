#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN_WINDOW_CPP="$ROOT_DIR/src/app/MainWindow.cpp"
MAIN_WINDOW_H="$ROOT_DIR/src/app/MainWindow.h"
MAIN_CPP="$ROOT_DIR/src/main.cpp"

if ! grep -q 'QSettings' "$MAIN_WINDOW_CPP"; then
  printf 'main window should persist preferences with QSettings\n' >&2
  exit 1
fi

for symbol in loadPreferences savePreferences closeEvent; do
  if ! grep -q "$symbol" "$MAIN_WINDOW_H" "$MAIN_WINDOW_CPP"; then
    printf 'main window is missing preference hook: %s\n' "$symbol" >&2
    exit 1
  fi
done

for key in publish/endpoint publish/namespace video/width video/height video/framerate video/bitrate audio/sampleRate audio/channels audio/bitrate audio/codec fragment/durationMs fragment/targetBytes m2ts/program use/openh264 use/libav use/libopusFallback; do
  if ! grep -q "$key" "$MAIN_WINDOW_CPP"; then
    printf 'preferences are missing key: %s\n' "$key" >&2
    exit 1
  fi
done

for forbidden in source/video source/audio source/camera source/microphone videoSource audioSource cameraDeviceId microphoneDeviceId; do
  if grep -q "settings\\.setValue(.*$forbidden" "$MAIN_WINDOW_CPP"; then
    printf 'preferences must not persist A/V source selection: %s\n' "$forbidden" >&2
    exit 1
  fi
done

if ! grep -q 'setOrganizationName' "$MAIN_CPP" || ! grep -q 'setApplicationName' "$MAIN_CPP"; then
  printf 'application should set QSettings identity before constructing MainWindow\n' >&2
  exit 1
fi
