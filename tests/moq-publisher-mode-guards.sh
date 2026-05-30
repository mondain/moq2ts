#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PUBLISHER_SOURCE="$ROOT_DIR/src/publish/MoqxrPublisher.cpp"
CONFIG_HEADER="$ROOT_DIR/src/app/PublishConfig.h"
MAIN_WINDOW="$ROOT_DIR/src/app/MainWindow.cpp"

if ! grep -q 'mock://' "$PUBLISHER_SOURCE"; then
  printf 'real moqxr builds should keep a mock:// endpoint mode for local pipeline testing\n' >&2
  exit 1
fi

if ! grep -q 'useMockTransport' "$PUBLISHER_SOURCE"; then
  printf 'publisher should route mock:// endpoints away from WebTransport\n' >&2
  exit 1
fi

if ! grep -q 'mock://local' "$CONFIG_HEADER"; then
  printf 'default publish config should avoid connecting to a relay until one is selected\n' >&2
  exit 1
fi

if ! grep -q 'mock://local' "$MAIN_WINDOW"; then
  printf 'default UI endpoint should avoid connecting to a relay until one is selected\n' >&2
  exit 1
fi
