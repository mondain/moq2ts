# Egress pacing on a common A/V timeline design

Status: design approved (sections), pending written-spec review
Date: 2026-06-01
Repo: moq2ts (this repo); Linux capture path. moqxr untouched.

## Problem

Live capture playback through the relay is choppy. Relay logs show objects
arriving in bursts: ~8 program-1 objects within ~20 ms, then a ~1.5 s idle gap,
repeating. A player gets a batch, plays it fast, then starves -> choppy.

Two compounding causes, both publisher-side:

1. Object emission is byte-driven and unpaced. `readObject` fills each object to
   `targetSegmentBytes` and returns it as fast as muxed bytes accumulate; the
   live path sets `publisherConfig.paced = false` so moqxr ships objects as fast
   as produced. The per-keyframe interleaver flush
   (`av_interleaved_write_frame(nullptr)` at each IDR) dumps a whole GOP's TS
   bytes at once, so a group's objects flush together, then nothing until the
   next keyframe.
2. Objects carry a synthetic media time. `LivePipeline` sets
   `mediaTimeUs = objects * fragmentDurationMs * 1000` (object index x a config
   constant), not the real encoder PTS. So even moqxr's existing `pace_until`
   (which gates on `media_time_us`) could not pace correctly against it.

Separately but related: audio and video are not on a common timeline. Video PTS
is rescaled from the V4L2 capture timestamp; audio PTS is a free-running sample
counter starting at 0 (`stream->nextAudioPts`). The two elementary streams have
independent zero points, so the mpegts PCR (from the video PID) and the audio
PTS do not agree -> A/V sync drift. Pacing on the video media time would worsen
this unless the streams share one clock.

## Decision

Implement app-side egress pacing in the capture path, gated on the real encoder
PTS, **on a unified A/V media timeline**. moqxr is untouched (`paced_` stays
false; we do not rely on `pace_until`). The file/M2TS path is unchanged.

Chosen behavior (from brainstorming):
- Pace app-side (in `LibavCaptureSource`/`LivePipeline`), not in moqxr.
- Gate each object on the media time of the most-recent video frame whose TS
  bytes are in that object.
- Smooth playback with minimal added latency: release on media time with a small
  slack; objects that are due or overdue pass through immediately (never add
  catch-up delay).
- Establish a single capture epoch so audio and video PTS (and thus PCR) share
  one timeline.

## Design

### A. Common A/V timeline (prerequisite)

**A1. Shared capture epoch.** New `Impl` member:
```cpp
std::atomic<std::int64_t> captureEpochUs{-1};  // wall-clock us of first encoded frame
```
Set once via CAS by whichever stream encodes first, from a steady-clock reading
taken at frame arrival. Using a steady-clock delta (not the device timestamp's
arbitrary origin) gives both streams the same origin and is robust to V4L2 and
Pulse using different timestamp bases.

**A2. Video PTS rebased to the epoch** (`encodeFrame`, replacing the current
`best_effort_timestamp` rescale at lines ~1047-1062):
- On the first frame, latch `captureEpochUs` from steady-clock `now()`.
- `mediaUs = nowSteadyUs() - captureEpochUs`.
- `frame->pts = av_rescale_q(mediaUs, {1, 1000000}, encoder->time_base)`.
- Keep the existing `nextObjectId` fallback only when no clock is available.

**A3. Audio PTS rebased to the same epoch** (`encodeAudioFrame`, replacing the
free-running `nextAudioPts` counter at lines ~999-1000):
- Anchor the first audio encoder frame to the epoch, then advance by
  `nb_samples` (sample-accurate cadence), offset so audio and video share 0:
  `pts = llround((firstAudioWallUs - captureEpochUs) * sample_rate / 1e6) + accumulatedSamples`.
- This keeps audio sample-accurate AND epoch-aligned, avoiding FIFO-batching
  jitter while sharing the video clock.

**A4. PCR.** The mpegts muxer derives PCR from `pcrPid` (video, PID 256)
automatically. Once both PES streams share the epoch, PCR and both PTS agree; no
explicit PCR code. Verified via the demuxer check in Testing.

### B. PTS-driven egress pacing

**B1. Per-offset media-time map.** New `Impl` member:
```cpp
std::deque<std::pair<std::uint64_t, std::uint64_t>> offsetMediaUs; // (muxedByteOffset, videoMediaUs)
```
In `sendEncoderFrame`, for each **video** packet written, after the interleaver
settles, push `(muxedConsumed + muxedBytes.size(), mediaUs)` reusing the offset
arithmetic the keyframe flush already computes. Entries are kept monotonic in
both offset and mediaUs. Audio packets do not drive pacing.

**B2. Tag each object** (`readObject`). When an object is sliced ending at
absolute offset `end`, set `object.mediaTimeUs` to the mediaUs of the latest
`offsetMediaUs` entry with offset <= `end`, popping consumed entries (same
lifecycle as `groupBoundaries`). This real media time replaces the synthetic
`objects * fragmentDurationMs` value in `LivePipeline::nextObject` and also
corrects the timeline track's `mediaTimeUs`.

**B3. Pace the producer** (`LivePipeline` capture `nextObject`, after
`readObject` returns). Extract the decision as a pure function for testability:
```cpp
// returns microseconds to sleep before releasing the object (0 = release now)
std::int64_t paceDelayUs(std::int64_t objectMediaUs, std::int64_t elapsedUs, std::int64_t slackUs);
//   delay = max(0, objectMediaUs - slackUs - elapsedUs)
```
Caller latches `pacingStartUs` (steady-clock) on the first object and loops:
```cpp
while (m_running.load(acquire)) {
    const std::int64_t delay = paceDelayUs(object.mediaTimeUs, nowSteadyUs() - pacingStartUs, slackUs);
    if (delay <= 0) break;
    sleep for min(delay, 5000) us;   // short, stop-checkable steps
}
```
- `slackUs` small (default ~8000 us) keeps steady-state latency near zero: only
  objects arriving ahead of their media time wait; due/overdue objects pass
  straight through.
- Sleeping in <=5 ms increments re-checking `m_running` preserves the bounded
  stop behavior (Stop stays responsive); on stop the producer returns nullopt.
- `pacingStartUs` and `captureEpochUs` are both wall/steady-relative; an object's
  `mediaTimeUs` is epoch-relative, so the comparison is consistent.

**B4. Config.** New `bool paceEgress = true` on PublishConfig (default on for
capture; lets pacing be disabled for debugging). No new UI required.

## Data flow

```
[first frame of either stream]
  captureEpochUs.CAS(-1 -> nowSteadyUs())              [A1]

[per video frame] encodeFrame:
  mediaUs = nowSteadyUs() - epoch; frame->pts = rescale(mediaUs)   [A2]
  sendEncoderFrame: write H.264 -> push (offset, mediaUs)          [B1]

[per audio frame] encodeAudioFrame:
  pts = epoch-anchored sample count                                 [A3]
  sendEncoderFrame: write AAC -> same mpegts (PCR+PTS agree)        [A4]

[object production] readObject:
  slice object ending at offset E; object.mediaTimeUs = offsetMediaUs[<=E]  [B2]
LivePipeline nextObject:
  latch pacingStartUs on first object
  while m_running && paceDelayUs(mediaTimeUs, elapsed, slack) > 0: sleep <=5ms  [B3]
  return object -> publish_live_objects -> relay

Result: objects leave spread across each GOP's real duration, A/V sync'd,
instead of a per-GOP burst then idle.
```

The pacing wait is the only new blocking point; bounded and m_running-checked.
Downstream (publish loop, moqxr, relay) unchanged.

## Error handling

- Epoch unset when tagging (no frame encoded yet): object falls back to the
  prior synthetic mediaTime and pacing is skipped for it (treated as due now).
  Never blocks on a missing clock.
- Frame without usable PTS: existing `nextObjectId` fallback stays; offsetMediaUs
  is clamped monotonic so pacing never sleeps on a garbage (negative/huge) value.
- Overdue object (clock hiccup): `paceDelayUs` returns 0 -> release immediately;
  pacing only holds early objects, never adds catch-up delay.
- Stop during a pace wait: loop checks `m_running` every <=5 ms and bails,
  returning nullopt so the publish loop unwinds. No unbounded sleep.
- Audio-only stream: no video map; tag from audio media time, or skip pacing if
  neither clock is present. Video-only: audio path absent, unaffected.
- Epoch race: `captureEpochUs` set by atomic CAS; first encoder wins, both
  streams rebase against the same value. No torn/double init.
- Map growth: `offsetMediaUs` entries popped as objects consume offsets (same
  lifecycle as `groupBoundaries`); cannot grow unbounded.

## Testing

1. Unit test (pure logic, no device): `paceDelayUs(mediaUs, elapsedUs, slackUs)`
   in a standalone `main()`+`expect` test wired like `tests/v4l2_selection_test.cpp`:
   - ahead of clock -> positive delay ~= mediaUs - slack - elapsed
   - within slack -> 0
   - overdue (negative) -> 0 (no catch-up delay)
   - monotonic media times -> non-decreasing due times
2. Independent A/V-timeline verification: capture a few seconds of our exact
   muxed TS to a file (the ffprobe-on-a-file technique), assert with
   `ffprobe -show_packets`: video and audio first-PTS within a small delta
   (shared epoch), PCR present and monotonic on the video PID, audio PTS cadence
   sample-accurate. Pre-fix shows the epoch mismatch; post-fix they align.
3. Structural guards (bash) + Docker build: extend
   `tests/libav-capture-source-guards.sh` to assert `captureEpochUs` exists and
   is CAS-set, audio PTS uses the epoch anchor (not a bare from-zero counter),
   `offsetMediaUs` is populated in `sendEncoderFrame`, `readObject` tags
   `mediaTimeUs` from it (not `objects * fragmentDurationMs`), and the pace wait
   is `m_running`-checked. Docker bookworm build as compile/link gate.
4. Manual acceptance: publish to the relay; from red5.log confirm object-arrival
   cadence is smooth (objects spread across each ~GOP interval, not 8-in-20ms
   then ~1.5s idle, directly comparable to the measured burst pattern); playback
   no longer choppy; A/V in sync.

Honest limitation: the device-capture PTS rebasing (A2/A3) and the offset->mediaUs
threading cannot be unit-tested without hardware; covered by the demuxer check
(2) + guards (3) + manual run (4) -- the same coverage level as the rest of the
capture path. The pure pacing math (1) gets real tests.

## Files

- `src/app/PublishConfig.h` - add `bool paceEgress = true`.
- `src/media/LibavCaptureSource.h/.cpp` - `captureEpochUs`; video PTS rebase
  (A2); audio PTS epoch anchor (A3); `offsetMediaUs` map populated in
  `sendEncoderFrame` (B1); `readObject` tags `mediaTimeUs` (B2); `paceDelayUs`
  pure helper.
- `src/media/LivePipeline.cpp` - capture `nextObject` uses the object's real
  `mediaTimeUs` and applies the `paceDelayUs` wait (B3); stop-checked.
- `tests/v4l2_selection_test.cpp` sibling or a new `tests/pace_delay_test.cpp`
  + CMake target - unit test for `paceDelayUs`.
- `tests/libav-capture-source-guards.sh` - structural guards.

## Out of scope (recorded)

- moqxr `pace_until` / `paced_` path (left off; pacing is app-side).
- The relay-side TSHandler ingest stall (separate red5pro-plugins investigation;
  documented in /tmp/red5pro-msfts/TSHANDLER-MSFTS-FINDINGS.md).
- File/M2TS source pacing (the burst is a live-capture artifact).
- Adaptive bitrate / encoder rate control changes.

## Verification

- `paceDelayUs` unit test passes (table cases).
- Demuxer check: video/audio first-PTS aligned, PCR monotonic on video PID.
- `tests/libav-capture-source-guards.sh` passes; Docker bookworm build links.
- Manual: relay object cadence smooth (no per-GOP burst/idle); playback smooth,
  A/V synced.
