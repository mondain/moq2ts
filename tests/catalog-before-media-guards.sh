#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
moqxr_session="$repo_root/../moqxr/src/transport/moqt_session.cpp"

python3 - "$moqxr_session" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()

if "bool live_object_catalog_sent =" not in source:
    raise SystemExit("live object publishing must explicitly track whether catalog has been sent")

if 'next->track_name == "catalog"' not in source:
    raise SystemExit("live object publishing must recognize the catalog track specially")

preannounce_condition = "if (!uses_request_streams(draft_version))"
if preannounce_condition not in source:
    raise SystemExit("live object publishing must preannounce tracks before forwarding objects")

old_preannounce_condition = "if (!uses_request_streams(draft_version) && !auto_forward_)"
live_object_section = source.split("TransportStatus MoqtSession::publish_live_objects(const openmoq::publisher::LiveObjectSource& source,", 1)[1]
if old_preannounce_condition in live_object_section:
    raise SystemExit("live object publishing must preannounce tracks even when auto-forward is enabled")

catalog_block = source.split('next->track_name == "catalog"', 1)[1].split("continue;", 1)[0]
if "live_object_catalog_sent = true;" not in catalog_block:
    raise SystemExit("catalog publish path must mark catalog as sent before media can drain")

media_guard = "if (auto_forward_ && !live_object_catalog_sent)"
if media_guard not in source:
    raise SystemExit("auto-forward live object publishing must wait for catalog before media")

guard_index = source.index(media_guard)
serve_index = source.index("sender_by_track[next->track_name].serve", guard_index)
if guard_index > serve_index:
    raise SystemExit("catalog-before-media guard must run before serving media objects")
PY
