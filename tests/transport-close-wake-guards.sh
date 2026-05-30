#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
picoquic_client="$repo_root/../moqxr/src/transport/picoquic_client.cpp"
webtransport_client="$repo_root/../moqxr/src/transport/webtransport_client.cpp"

python3 - "$picoquic_client" "$webtransport_client" <<'PY'
from pathlib import Path
import sys

for path_arg in sys.argv[1:]:
    path = Path(path_arg)
    source = path.read_text()

    if "impl_->close_requested" not in source:
        raise SystemExit(f"{path.name} must expose close_requested to blocking transport waits")

    wait_blocks = source.split("condition.wait_for")
    if len(wait_blocks) < 3:
        raise SystemExit(f"{path.name} must have blocking wait_for calls to guard")

    for marker in [
        "TransportStatus " + ("PicoquicClient" if "picoquic" in path.name else "WebTransportClient") + "::accept_stream",
        "TransportStatus " + ("PicoquicClient" if "picoquic" in path.name else "WebTransportClient") + "::read_stream",
    ]:
        if marker not in source:
            raise SystemExit(f"{path.name} missing guarded method {marker}")
        section = source.split(marker, 1)[1].split("\nTransportStatus ", 1)[0]
        if "close_requested" not in section:
            raise SystemExit(f"{path.name} {marker} must wake when close is requested")
        if "transport close requested" not in section:
            raise SystemExit(f"{path.name} {marker} must return a close-requested status instead of waiting for timeout")
PY
