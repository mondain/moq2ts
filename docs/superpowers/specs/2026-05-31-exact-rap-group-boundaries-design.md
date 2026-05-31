# Exact RAP-based group boundaries (TS scan)

Status: design approved, not yet implemented
Date: 2026-05-31

## Problem

The keyframe-aligned grouping feature records group boundaries at ENCODE time:
`sendEncoderFrame` captures `beforeSize = muxedProduced` when an `AV_PKT_FLAG_KEY`
video packet is submitted to `av_interleaved_write_frame`. The interleaver buffers
and reorders across audio+video, so the submit-time byte offset does not reliably
equal where the keyframe's TS packets actually land in `muxedBytes`. With A/V the
recorded boundary can be off by one or more frames, weakening the
`m2tsRandomAccess: true` guarantee (final-review Important finding).

## Goal

Make each `groupBoundaries` entry land exactly on the first TS packet of a video
random-access point, regardless of interleaving — by scanning the muxed byte
stream after bytes have landed, instead of guessing at encode time.

## Key insight

Task 3's `readObject` already slices correctly on `groupBoundaries` (absolute
byte offsets) and assigns group/object IDs, `startsGroup`, etc. Only the SOURCE of
those offsets is imprecise. So keep `readObject` as-is and replace the boundary
source.

## Changes (src/media/LibavCaptureSource.cpp only)

### Remove (encode-time, imprecise)
In `sendEncoderFrame`, drop the `isVideoKey` / `beforeSize` /
`groupBoundaries.push_back(...)` / `sawVideoKeyframe` logic and the
`muxedProduced` accounting. The `muxedProduced` member is removed entirely. The
write block returns to: rescale ts, set stream_index, `av_interleaved_write_frame`,
error-check.

### Add (byte-accurate scan)
- New Impl members: `int videoPidValue = -1;` and `std::uint64_t scannedTo = 0;`
  (absolute offset up to which the muxed stream has been scanned for RAPs).
- Capture the video PID: right after `avformat_write_header` succeeds, set
  `videoPidValue` from the video output stream's assigned id
  (`stream->outputStream->id` for the video StreamState). If unavailable, leave
  -1 and fall back to `pcrPidValue` in the scan.
- New helper `void scanForRapBoundaries()` (Impl member), called at the end of
  `pumpOnce` after packets have been appended (and once before slicing in
  `readObject` is acceptable, but `pumpOnce` is the single append site so call it
  there):
  - Scan packet-aligned 188-byte TS packets in the absolute window
    `[max(scannedTo, muxedConsumed) .. muxedConsumed + muxedBytes.size())`.
    `muxedBytes` is always packet-aligned because `readObject` only removes
    188-byte multiples.
  - For each packet at absolute offset A (buffer index `A - muxedConsumed`):
    - require sync byte 0x47;
    - `pid = ((b1 & 0x1f) << 8) | b2`;
    - `pusi = b1 & 0x40`;
    - adaptation control `(b3 >> 4) & 0x3` in {2,3}, `adaptation_field_length`
      (b4) > 0, `random_access_indicator = b5 & 0x40`;
    - RAP = `pid == effectiveVideoPid && pusi && adaptation && RAI`.
  - effectiveVideoPid = `videoPidValue >= 0 ? videoPidValue : pcrPidValue`.
  - On the FIRST RAP seen: set `sawVideoKeyframe = true` but do NOT push it
    (group 0 starts at byte 0 and contains the first IDR — preserves existing
    semantics and the old "skip first boundary" guard intent).
  - On every subsequent RAP: `groupBoundaries.push_back(A)`.
  - Advance `scannedTo` to the end of the scanned region so packets are never
    rescanned.

Note: 192-byte M2TS source packets are not produced by this capture muxer (it
writes 188-byte TS), so the scan assumes 188; guard with the existing packet size
if needed but capture is always 188 here.

### Unchanged
`readObject` slicing/grouping, `nextGroupId`/`nextObjectIdInGroup`/
`pendingGroupBoundary`, `startsGroup`, `hasVideoStream`,
`randomAccessActive() = hasVideoStream && sawVideoKeyframe`, the catalog emission,
and the pipeline wiring all stay exactly as committed.

## Test

`tests/keyframe-aligned-groups-guards.sh` currently asserts `AV_PKT_FLAG_KEY`
appears in the capture source. That token is being removed, so update that
assertion to instead assert the RAI-scan invariants, e.g.:
- `random_access_indicator` (or the `0x40` RAI mask with a scan) present in
  capture,
- `scanForRapBoundaries` present,
- `groupBoundaries` still present,
- keep the existing `startsGroup`, `randomAccessActive`, and
  `if (catalog.randomAccess)` assertions.

## Risks / notes

- effectiveVideoPid must be the muxer-assigned PID. Confirm `outputStream->id`
  holds the TS PID after `avformat_write_header` (it does for mpegts); if it is 0
  / unset, the pcrPidValue fallback covers the common single-program case where
  PCR rides the video PID.
- The MPEG-TS muxer emits PAT/PMT before an IDR at stream start and may repeat PSI
  before random-access points; the scanned RAP offset points at the video RAP
  packet itself. Subscribers needing PSI at the group join rely on initData /
  periodic PSI; aligning the group to the video RAP packet is the spec
  requirement (MSFTS 5.2/5.3). PSI-before-IDR refinement is out of scope here.
- This supersedes the encode-time portion of the prior Task 2; implemented as a
  new commit on top of the existing history (no history rewrite).
