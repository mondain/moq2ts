# Keyframe-flagged group boundaries (replace TS-byte RAP scan)

Status: design approved, not yet implemented
Date: 2026-05-31

## Problem

The TS-byte RAP scanner (`scanForRapBoundaries`) does not work: with the GOP fix
in place the encoder emits periodic IDRs (libx264 `frame I:9..13` in publisher
stderr, ~1s apart, correct bitrate), yet the scanner printed NO diagnostic at all
and the relay still received a single `group=0` (225 objects). The scanner keys on
the MPEG-TS adaptation-field `random_access_indicator` bit, which FFmpeg's
mpegtsenc does not set the way the scanner assumes for libx264 output, so zero
RAPs are detected. Two boundary-detection attempts have now failed (submit-time
offset = imprecise; TS RAI scan = never fires).

## Decision

Stop parsing TS bytes to find keyframes. Use the encoder's authoritative
`AV_PKT_FLAG_KEY` on each encoded packet, and make the byte offset exact by
flushing the muxer's interleaver at the keyframe so the boundary lands on the IDR.
This is encoder-agnostic (libx264 and libopenh264) and removes all guessing about
muxer bit semantics.

## Design

### Boundary detection in sendEncoderFrame (the write loop)

For each encoded packet, after `av_packet_rescale_ts` / `stream_index`, before the
normal `av_interleaved_write_frame(outputFormat, packet)`:

```
isVideoKey = stream->video && (packet->flags & AV_PKT_FLAG_KEY)
if isVideoKey:
    av_interleaved_write_frame(outputFormat, nullptr)   // flush queued packets
    offset = muxedConsumed + muxedBytes.size()          // empty queue => next bytes are the IDR
    if !sawVideoKeyframe:
        sawVideoKeyframe = true                          // first IDR: group 0 contains it; no push
        fprintf(stderr, "[moqxr][diag] keyframe groups: first IDR at offset=%llu\n", offset)
    else:
        groupBoundaries.push_back(offset)
        ++rapBoundaryCount
        if rapBoundaryCount % 30 == 1:
            fprintf(stderr, "[moqxr][diag] keyframe boundaries=%llu latest offset=%llu\n", rapBoundaryCount, offset)
then write the keyframe packet normally.
```

`av_interleaved_write_frame(ctx, nullptr)` is the documented way to flush buffered
interleaved packets mid-stream; after it the interleaver queue is empty, so
`muxedConsumed + muxedBytes.size()` is the exact absolute byte offset where the
keyframe's TS packets begin. The boundary is byte-exact -> `m2tsRandomAccess:true`
is honest.

### readObject unchanged

The committed `readObject` already consumes `groupBoundaries` (boundary-aware
slicing + per-group object IDs + `startsGroup` + `pendingGroupBoundary`). No change
needed. Threading is safe: `pumpOnce` -> `sendEncoderFrame` and `readObject` run on
the single capture worker thread, so the `groupBoundaries` deque and
`sawVideoKeyframe`/`rapBoundaryCount` members are race-free.

### Remove the dead byte-scanner

Delete:
- `void scanForRapBoundaries()` (entire method),
- its call at the end of `pumpOnce`,
- the `std::uint64_t scannedTo = 0;` member,
- the `int videoPidValue = -1;` member and its capture block after
  `avformat_write_header` (the `for (const auto& s : streams) { ... videoPidValue =
  s->outputStream->id; }`).

Keep: `groupBoundaries`, `muxedConsumed`, `nextGroupId`, `sawVideoKeyframe`,
`hasVideoStream`, `nextObjectIdInGroup`, `pendingGroupBoundary`, `rapBoundaryCount`.

### Diagnostic

The throttled `[moqxr][diag]` stderr now lives in `sendEncoderFrame` (above). It
WILL fire because encoded keyframes definitely occur (unlike the dead scanner). It
distinguishes states for the next run: a "first IDR" line then climbing
"keyframe boundaries=N".

### Guard test

`tests/keyframe-aligned-groups-guards.sh` currently asserts `scanForRapBoundaries`,
`random_access_indicator`, and `videoPidValue` -- all removed. Replace those three
assertions with:
- `AV_PKT_FLAG_KEY` present in capture (keyframe detection),
- `groupBoundaries.push_back` present in capture (boundary recording).
Keep the existing assertions: `bool startsGroup`, `object->startsGroup = true;`,
`randomAccessActive` (capture + header), `if (catalog.randomAccess)`,
`keyframeIntervalMs`, `gop_size = gopSize`.

## Caveat (recorded, accepted)

Flushing the interleaver at each keyframe slightly perturbs A/V interleaving at GOP
boundaries (audio queued just before the IDR is flushed ahead of it). For grouping
this is benign/desirable (clean group separation). Noted, not a defect.

## Out of scope (recorded)

- The ~60 s relay inactivity close.
- The relay subscriber queue-overflow (a downstream symptom of the single-group
  problem; expected to clear once groups exist).
- The catalog cold-start `m2tsRandomAccess` omission (catalog built before first
  keyframe) -- prior documented limitation.

## Files

- `src/media/LibavCaptureSource.cpp` - keyframe-flag boundary + diag in
  `sendEncoderFrame`; remove `scanForRapBoundaries`, its `pumpOnce` call,
  `scannedTo`, `videoPidValue` + its capture.
- `tests/keyframe-aligned-groups-guards.sh` - swap scanner assertions for
  keyframe-flag assertions.

## Verification

Rebuild; run a capture publish capturing PUBLISHER stderr.
- Success: stderr shows `[moqxr][diag] keyframe groups: first IDR at offset=...`
  then `keyframe boundaries=N ...` climbing; relay log shows objects in groups
  beyond 0 (`program-1/1/...`); stream becomes playable.
