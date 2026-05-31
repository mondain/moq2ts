# Keyframe-aligned MOQT groups for honest m2tsRandomAccess

Status: design approved, not yet implemented
Date: 2026-05-31

## Problem

`LibavCaptureSource::readObject()` slices objects purely by byte count
(`targetSegmentBytes`) and hard-codes `object->groupId = 0`. It has no awareness
of H.264 access-unit or IDR boundaries. This violates several spec expectations:

- MSFTS Section 5.2 / 5.3 (draft-gregoire-moq-msfts-00): each MOQT Group should
  begin at a random-access point, and for live a new Group SHOULD start at each
  RAP. We satisfy neither.
- MSF Section 7.3 (draft-ietf-moq-msf-00): the first timeline object of each
  Group must be independent. Meaningless while there is only group 0.

Because of this, `m2tsRandomAccess: true` would be a false claim and is currently
omitted from the catalog (see commit ed61cb1).

## Goal

Start a new MOQT group at each IDR/keyframe so the first object of every group
begins at a random-access point. Then advertise `m2tsRandomAccess: true` and make
per-group timeline independence real.

## Approach (favored): record keyframe byte-offsets during muxing

Detect the keyframe at **encode time** (authoritative) and communicate the
boundary to the byte-slicing layer across the muxing boundary.

Pipeline today: frames encoded -> fed to MPEG-TS muxer
(`av_interleaved_write_frame`) -> bytes land in `muxedBytes` (an `avio` memory
sink via `writePacket`) -> `readObject` slices that byte buffer.

1. Capture keyframe boundaries. When an `AVPacket` carrying `AV_PKT_FLAG_KEY` is
   written, record the current `muxedBytes` length as a group-boundary offset in
   a queue (e.g. `std::deque<qint64> m_groupBoundaries`). The offset must be
   captured post-mux (inside/around the write callback), not at `send_frame`
   time, because the interleaver may buffer and reorder.

2. Slice on boundaries in `readObject`. An object must never cross a group
   boundary:
   - If the next pending boundary falls within the current target window, cut the
     object at the boundary so the next object (and new group) starts exactly at
     the RAP.
   - Increment `m_groupCounter` when a new group starts; assign
     `object->groupId = m_groupCounter`, and reset `object->objectId = 0` at each
     group start (object IDs increase within a group, MSFTS 5.3).

3. First-object-of-group flag. Carry `bool startsGroup` on
   `M2tsObject`/`PublishedObject` so the publisher can set group-largest /
   first-in-group semantics and the timeline path emits an independent record per
   group (MSF 7.3).

4. Catalog. Plumb a `randomAccess` field through `MsftsCatalog` and set
   `m2tsRandomAccess: true` only when keyframe grouping is actually active
   (capture path with video), never blindly.

5. Fallback. If there is no video stream (audio-only) or no keyframe seen yet,
   fall back to the current behavior (single growing group, `m2tsRandomAccess`
   omitted) so we never lie.

## Alternatives considered

- Parse TS for RAI/PUSI in `readObject` (no encode-side hook): self-contained but
  fragile (locate video PID PES start + SPS/IDR, handle 188/192, adaptation
  fields). More bug surface. Rejected unless muxer reordering makes offset
  tracking unreliable.
- One GOP per object (object == group): simplest grouping but couples object size
  to GOP size and ignores `targetSegmentBytes`. Rejected; keep byte-target
  slicing within a group.

## Risks / unknowns to verify during implementation

- Muxer reordering/buffering: confirm the recorded byte offset corresponds to
  where the keyframe's packets actually land in `muxedBytes`.
- PSI placement: ideally PAT/PMT precede the IDR at a group start (MSFTS 6.11 /
  5.2). May need the muxer to emit PSI at each keyframe so a joiner has tables.
- GOP size vs. fragmentDuration: group cadence becomes the encoder GOP, not
  `fragmentDurationMs`; timeline interval logic (`timelineEveryObjects`) should
  key off group starts, not a fixed object count.
- File-source path (`M2tsPacketizer`) is separate and would need its own RAP
  detection (TS parse); deferred as a follow-up beyond the capture path.

## Scope

Capture path only:
- `LibavCaptureSource.{h,cpp}` - boundary queue, slicer, group/object counters,
  key-flag capture.
- `M2tsObject` / `PublishedObject` - add `startsGroup`.
- `MsftsMuxer.{h,cpp}` - add `randomAccess` field, emit `m2tsRandomAccess`.
- `LivePipeline.cpp` - set catalog flag, group-aware timeline emission.

File-source RAP detection deferred.
