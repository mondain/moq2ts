# Live vs VOD catalog handling

Status: design approved, not yet implemented
Date: 2026-05-31

## Goal

Distinguish live from video-on-demand (VOD) publishes and emit a spec-correct
catalog for each, per draft-ietf-moq-msf-00 (the `isLive`-conditional fields) and
draft-gregoire-moq-msfts-00 Section 7.3 (the VOD transport-stream example).

## Source-mode detection

Capture is always live; a file/URL source is VOD.

- `LivePipeline::runLoop()` capture branch (camera/mic via `LibavCaptureSource`)
  -> `isLive = true`.
- `LivePipeline::runLoop()` file branch (via `M2tsPacketizer`) -> `isLive = false`.

No UI change and no new `PublishConfig` field; the mode is a function of which
branch runs. Known limitation: a live file-pipe or live URL fed through the file
branch would be mislabeled VOD. Acceptable for now.

## Catalog field differences

Verified against the drafts:

| Field | Live | VOD | Rule |
|-------|------|-----|------|
| `isLive` (track) | `true` | `false` | MSF 5.1.15, Required |
| `generatedAt` (root) | emit | omit | MSF 5.1.6: SHOULD NOT if isLive false |
| `targetLatency` (track) | emit | omit | MSF 5.1.16: MUST NOT if isLive false |
| `trackDuration` (track, ms) | omit | emit if known | MSF 5.1.37: MUST NOT if isLive true |
| `m2tsRandomAccess` (track) | omit (today) | `true` | MSFTS 6.8 / 7.3 example |

`MsftsMuxer::catalogJson` already gates `generatedAt` and `targetLatency` on
`isLive`. This work adds the `trackDuration` field (gated to VOD) and the
`m2tsRandomAccess` field. Note: `MsftsCatalog` does NOT yet carry a `randomAccess`
member in committed code (that was specified only in the parked keyframe-aligned
groups plan); this work introduces it.

For live, `m2tsRandomAccess` is omitted for now (capture grouping is not yet
keyframe-aligned, so it would be false regardless). For VOD it is set `true`.

## trackDuration source

`M2tsPacketizer` reads raw TS and cannot report a duration. Add an isolated probe
that opens the file with libav (`avformat_open_input` + `avformat_find_stream_info`),
reads `AVFormatContext::duration` (microseconds), converts to integer
milliseconds, and closes it. Called once at catalog build on the file path. On any
failure it returns 0 and `trackDuration` is omitted -- still a valid VOD catalog.

The probe must not disturb the existing raw-TS read loop (separate
AVFormatContext, opened and closed within the helper).

## Caveat (recorded, accepted)

`m2tsRandomAccess: true` for VOD is slightly optimistic: the file path still
slices objects by byte target into a single group 0, so we do not yet guarantee
each MOQT group begins at a random-access point even though the asset's own IDRs
exist in the payload. Emitting it is an explicit decision; making it truthful is
covered by extending the keyframe-aligned-groups work to the file path (parked).

## Files

- `MsftsMuxer.h` - add `qint64 trackDurationMs = 0;` and `bool randomAccess =
  false;` (`isLive` already present).
- `MsftsMuxer.cpp` - emit `trackDuration` when `!isLive && trackDurationMs > 0`;
  emit `m2tsRandomAccess: true` when `randomAccess` is true.
- `M2tsPacketizer.h` / `.cpp` - add `static qint64 probeDurationMs(const QString&
  sourcePath)` (or a file-local helper) using libav, guarded by the existing
  libav build macro; return 0 on failure or when libav is unavailable.
- `LivePipeline.cpp` - capture branch sets `isLive = true`; file branch sets
  `isLive = false`, `randomAccess = true`, and `trackDurationMs` from the probe.
- New `tests/live-vod-catalog-guards.sh` source guard.
