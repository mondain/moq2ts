# Configurable keyframe interval + RAP-scan diagnostic

Status: design approved, not yet implemented
Date: 2026-05-31

## Problem

Relay logs from a working publish show the capture stream is received and ingested
(124 objects, ~8 MB, TSMuxer created) but is not playable. Every object arrives as
`program-1/0/N` -- group 0 only, no group boundaries -- so a mid-stream player has
a single random-access point and the relay's TSHandler (skipAU=true) never gets a
clean access unit.

Root cause confirmed in code: the H.264 encoder's `gop_size` is never set
(`grep gop_size` = 0), so there is no guaranteed periodic IDR. Without periodic
IDRs the RAP scanner finds no boundaries after the first keyframe and grouping
correctly produces one ever-growing group. A bounded GOP (frequent IDRs) is
required for live random access regardless of the scan.

A secondary unknown: we cannot tell from logs whether the RAP scanner found zero
RAPs or scanned the wrong PID, because there is no publisher-side scanner
diagnostic and the captured console was the relay's output, not the publisher's.

## Goals

1. Enforce a bounded, configurable IDR cadence on the live encoder so periodic
   keyframes exist (and thus group boundaries).
2. Emit a throttled publisher-side diagnostic from the RAP scanner so the next run
   proves whether grouping fires and on which PID.

## Design

### 1. Config field

`src/app/PublishConfig.h`: add
```cpp
int keyframeIntervalMs = 1000;  // live IDR cadence; group cadence follows it
```
Default 1000 ms (1 s). No UI control in this change (defaults apply); a UI knob is
a possible follow-up.

### 2. Encoder IDR cadence (LibavCaptureSource::addVideo)

The selected encoder is libopenh264 (primary) with libx264 as fallback. Before
`avcodec_open2`, compute and apply a closed GOP:
```cpp
const int fr = std::max(1, config.videoFramerate);
int gop = static_cast<int>((static_cast<long long>(fr) * std::max(1, config.keyframeIntervalMs) + 500) / 1000);
gop = std::max(1, gop);
stream->encoder->gop_size = gop;
stream->encoder->max_b_frames = 0;  // closed, low-latency GOP
```
Belt-and-suspenders so both encoders honor a fixed period (unknown options must be
non-fatal -- use the AV_OPT_SEARCH_CHILDREN search and ignore failures; do NOT
error if an option is absent for the active encoder):
```cpp
// libx264 fallback: pin keyint and disable scene-cut so IDRs land on a fixed period.
av_opt_set_int(stream->encoder->priv_data, "keyint", gop, 0);
av_opt_set_int(stream->encoder->priv_data, "min-keyint", gop, 0);
av_opt_set(stream->encoder->priv_data, "sc_threshold", "0", 0);
```
These calls return an error code when the active encoder (libopenh264) lacks the
option; that return is intentionally ignored (not assigned to the fatal `rc`).
libopenh264 honors `gop_size` directly. The existing `bit_rate` cap and
`profile=baseline` are unchanged.

### 3. RAP-scan diagnostic (scanForRapBoundaries), throttled

Add a per-Impl counter `std::uint64_t rapBoundaryCount = 0;`. In
`scanForRapBoundaries`:
- Once, when the first RAP is detected (the existing `!sawVideoKeyframe` branch),
  log the PID being scanned:
  `std::fprintf(stderr, "[moqxr][diag] RAP scan: videoPid=%d pcrPid=%d\n", videoPid, pcrPidValue);`
- Each time a boundary is pushed, `++rapBoundaryCount;` and every 30th push log:
  `std::fprintf(stderr, "[moqxr][diag] RAP boundaries=%llu latest group offset=%llu\n", (unsigned long long)rapBoundaryCount, (unsigned long long)pos);`
Include `<cstdio>` if not already present in the file.

This writes to publisher stderr (where console logs were captured before) and is
throttled to avoid flooding.

## Out of scope (recorded, not fixed here)

- The ~64 s idle QUIC session close ("Picoquic session 1 closed: errorCode=0")
  and relay teardown.
- The catalog cold-start `m2tsRandomAccess` omission (catalog built before the
  first keyframe is observed) -- the documented limitation from the prior design.

## Files

- `src/app/PublishConfig.h` - add `keyframeIntervalMs`.
- `src/media/LibavCaptureSource.cpp` - GOP/closed-GOP + x264 opts in `addVideo`;
  `rapBoundaryCount` member + throttled stderr in `scanForRapBoundaries`;
  `<cstdio>` include if needed.

## Verification

Rebuild (Docker bookworm, real moqxr). Re-run a capture publish; capture the
PUBLISHER stderr and the relay log.
- Success: publisher stderr shows `[moqxr][diag] RAP scan: videoPid=...` then
  `RAP boundaries=N` climbing; relay log shows objects in groups beyond 0
  (`program-1/1/...`, `program-1/2/...`) and the stream becomes playable.
- If boundaries stay 0 with a sensible videoPid, the scanner's RAP test is wrong;
  if videoPid is wrong, the PID capture/fallback is wrong -- the diagnostic
  distinguishes these.
