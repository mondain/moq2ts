# Egress Pacing on a Common A/V Timeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop bursty per-GOP egress (and the resulting choppy playback) by pacing capture-path object emission on the real encoder PTS, with audio and video rebased to a single capture epoch so PCR and both PES streams share one timeline.

**Architecture:** Establish a steady-clock capture epoch in `LibavCaptureSource`; rebase video and audio PTS to it (common A/V timeline). Record the most-recent video media time per muxed byte offset; `readObject` tags each object with a real `mediaTimeUs`; `LivePipeline` releases each object no earlier than its media time minus a small slack, sleeping in short stop-checkable steps. The pacing decision is a pure, unit-tested helper. moqxr is untouched; the file/M2TS path is unchanged.

**Tech Stack:** C++20, libav (capture/encode/mux), Qt 6, CMake + CTest, Docker bookworm build, bash structural guards, ffprobe for the A/V-timeline check.

**Spec:** `docs/superpowers/specs/2026-06-01-egress-pacing-av-timeline-design.md`

**Repo (absolute):** `/media/mondain/terrorbyte/workspace/github-moq/moq2ts`

---

## File Structure

- Create `src/media/EgressPacing.h` — pure, header-only: `nowSteadyUs()` and `paceDelayUs(objectMediaUs, elapsedUs, slackUs)`. No Qt/libav, so the unit test links nothing heavy.
- Create `tests/pace_delay_test.cpp` — standalone unit test for `paceDelayUs`.
- Modify `src/media/M2tsPacketizer.h` — add `std::uint64_t mediaTimeUs` to `M2tsObject`.
- Modify `src/app/PublishConfig.h` — add `bool paceEgress = true`.
- Modify `src/media/LibavCaptureSource.cpp` — capture epoch + video/audio PTS rebase (A); `offsetMediaUs` map + `readObject` tagging (B1/B2).
- Modify `src/media/LivePipeline.cpp` — use the object's real `mediaTimeUs` and apply the pace wait (B3).
- Modify `CMakeLists.txt` — add the `moq2ts-pace-delay-tests` target.
- Modify `tests/libav-capture-source-guards.sh` — structural guards.

---

## Task 1: Pure pacing helper + unit test

Self-contained, platform-independent. No device, no libav.

**Files:**
- Create: `src/media/EgressPacing.h`
- Create: `tests/pace_delay_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create src/media/EgressPacing.h**

```cpp
#pragma once

#include <chrono>
#include <cstdint>

namespace moq2ts {

// Microseconds on the monotonic steady clock. Use for deltas only (the absolute
// value is arbitrary). The capture epoch and the pacing start are both readings
// of this clock, so an object's media time (epoch-relative) and the elapsed time
// (start-relative) are comparable.
inline std::int64_t nowSteadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// How long (microseconds) to wait before releasing an object whose media time is
// objectMediaUs, given elapsedUs since the first object and a latency slack.
// Returns 0 when the object is due or overdue (never adds catch-up delay).
inline std::int64_t paceDelayUs(std::int64_t objectMediaUs, std::int64_t elapsedUs,
                                std::int64_t slackUs) {
    const std::int64_t delay = objectMediaUs - slackUs - elapsedUs;
    return delay > 0 ? delay : 0;
}

}  // namespace moq2ts
```

- [ ] **Step 2: Create tests/pace_delay_test.cpp**

```cpp
#include "media/EgressPacing.h"

#include <iostream>
#include <string>

using moq2ts::paceDelayUs;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}
}  // namespace

int main() {
    bool ok = true;

    // Ahead of clock: wait ~ mediaUs - slack - elapsed.
    ok &= expect(paceDelayUs(100000, 0, 8000) == 92000, "ahead -> positive delay");

    // Within slack: due now.
    ok &= expect(paceDelayUs(5000, 0, 8000) == 0, "within slack -> 0");

    // Exactly at media time minus slack boundary.
    ok &= expect(paceDelayUs(8000, 0, 8000) == 0, "boundary -> 0");

    // Overdue (elapsed past media time): no catch-up delay.
    ok &= expect(paceDelayUs(50000, 200000, 8000) == 0, "overdue -> 0");

    // Negative media time (garbage/clock-backward): never sleeps.
    ok &= expect(paceDelayUs(-1000, 0, 8000) == 0, "negative media -> 0");

    // Steady-state: object 1s out, 0.9s elapsed, 8ms slack -> ~92ms wait.
    ok &= expect(paceDelayUs(1000000, 900000, 8000) == 92000, "steady-state delay");

    return ok ? 0 : 1;
}
```

- [ ] **Step 3: Add the test target to CMakeLists.txt**

After the existing `moq2ts-v4l2-selection-tests` block (the `if(UNIX AND NOT APPLE) ... endif()` near the end of the file), append:

```cmake
# Pure egress-pacing delay logic test (no Qt/libav deps).
if(UNIX AND NOT APPLE)
    enable_testing()
    add_executable(moq2ts-pace-delay-tests
        tests/pace_delay_test.cpp
    )
    target_include_directories(moq2ts-pace-delay-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME moq2ts-pace-delay-tests COMMAND moq2ts-pace-delay-tests)
endif()
```

- [ ] **Step 4: Build and run the unit test**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
g++ -std=c++20 -I src tests/pace_delay_test.cpp -o /tmp/pace_test && /tmp/pace_test; echo "test_exit=$?"
```
Expected: `test_exit=0`, no FAIL lines.

- [ ] **Step 5: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/EgressPacing.h tests/pace_delay_test.cpp CMakeLists.txt
git commit -m "Add pure egress pacing-delay helper with unit tests"
```
Commit message EXACTLY that, NO Claude tagline/Co-Authored-By.

---

## Task 2: Common A/V timeline (capture epoch + video/audio PTS rebase)

Establish one steady-clock epoch and rebase both encoders' PTS to it.

**Files:**
- Modify: `src/media/LibavCaptureSource.cpp`

- [ ] **Step 1: Include the pacing header and ensure atomic/utility**

At the top of `src/media/LibavCaptureSource.cpp`, in the standard-include block (after `#include <cstdio>`), add:

```cpp
#include <atomic>
#include <utility>
```

And after `#include "V4l2Capabilities.h"` add:

```cpp
#include "EgressPacing.h"
```

(If `<atomic>` or `<utility>` is already present, do not duplicate.)

- [ ] **Step 2: Add the capture epoch member and ensureCaptureEpoch()**

In `struct LibavCaptureSource::Impl`, after the line `std::uint64_t rapBoundaryCount = 0;  // diagnostic: number of RAP boundaries recorded` (around line 225), add:

```cpp
    // Single capture epoch (steady-clock microseconds) shared by video and audio
    // so both PES streams and the mpegts PCR sit on one timeline. -1 until set
    // by the first encoded frame of either stream (CAS, set-once).
    std::atomic<std::int64_t> captureEpochUs{-1};
    // Ascending (muxedByteOffset, videoMediaUs) pairs: media time of the latest
    // video packet whose bytes end at/before each offset. Consumed by readObject.
    std::deque<std::pair<std::uint64_t, std::uint64_t>> offsetMediaUs;
    // Carries the media time of the last video packet consumed by readObject so
    // an object with no new video bytes reuses the last known media time.
    std::uint64_t lastVideoMediaUs = 0;
```

Then add a small helper method inside `Impl` (place it just before `bool readObject(...)` at line ~613):

```cpp
    void ensureCaptureEpoch() {
        std::int64_t expected = -1;
        captureEpochUs.compare_exchange_strong(expected, nowSteadyUs(), std::memory_order_acq_rel);
    }
```

- [ ] **Step 3: Add the per-stream audio anchor flag**

In `struct StreamState` (around line 228-242), after `int64_t nextAudioPts = 0;` add:

```cpp
        bool audioAnchored = false;
```

- [ ] **Step 4: Rebase video PTS to the epoch**

In `encodeFrame`, replace the current PTS block (lines ~1047-1062):

```cpp
        int64_t sourcePts = inputFrame->best_effort_timestamp;
        if (sourcePts == AV_NOPTS_VALUE) {
            sourcePts = inputFrame->pts;
        }
        if (sourcePts != AV_NOPTS_VALUE && stream->inputStream != nullptr) {
            // Rescale the capture PTS from the input (e.g. V4L2, microsecond)
            // timebase into the encoder timebase. Without this, libx264 sees
            // huge inter-frame gaps, disables effective rate control, and
            // encodes near-lossless -- flooding the transport far above the
            // configured bitrate until the QUIC connection idle-times-out.
            frame->pts = av_rescale_q(sourcePts,
                                      stream->inputStream->time_base,
                                      stream->encoder->time_base);
        } else {
            frame->pts = nextObjectId;
        }
        return sendEncoderFrame(stream, frame.get(), "video", error);
```

with:

```cpp
        // Stamp PTS from a single steady-clock capture epoch shared with audio,
        // so both streams (and the mpegts PCR) are on one timeline. The wall
        // delta also gives libx264 real, increasing inter-frame timing so rate
        // control works (the old raw best_effort_timestamp flooded the encoder).
        ensureCaptureEpoch();
        const std::int64_t mediaUs = nowSteadyUs() - captureEpochUs.load(std::memory_order_acquire);
        frame->pts = av_rescale_q(mediaUs, AVRational{1, 1000000}, stream->encoder->time_base);
        return sendEncoderFrame(stream, frame.get(), "video", error);
```

- [ ] **Step 5: Rebase audio PTS to the same epoch**

In `encodeAudioFrame`, replace (lines ~999-1000):

```cpp
            frame->pts = stream->nextAudioPts;
            stream->nextAudioPts += frame->nb_samples;
```

with:

```cpp
            // Anchor audio to the shared capture epoch on its first frame, then
            // advance sample-accurately. This keeps audio sample-exact while
            // starting from the same zero as video so A/V stay in sync.
            ensureCaptureEpoch();
            if (!stream->audioAnchored) {
                const std::int64_t epoch = captureEpochUs.load(std::memory_order_acquire);
                const std::int64_t startUs = (epoch >= 0) ? (nowSteadyUs() - epoch) : 0;
                stream->nextAudioPts = av_rescale(startUs, stream->encoder->sample_rate, 1000000);
                stream->audioAnchored = true;
            }
            frame->pts = stream->nextAudioPts;
            stream->nextAudioPts += frame->nb_samples;
```

- [ ] **Step 6: Build via Docker bookworm**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...moq2ts-publisher-bookworm`, exit 0.

- [ ] **Step 7: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/LibavCaptureSource.cpp
git commit -m "Rebase capture video and audio PTS to a shared epoch"
```
Commit message EXACTLY that, NO Claude tagline/Co-Authored-By.

---

## Task 3: Per-offset media-time map + object tagging

Record video media time per byte offset and tag each object with a real media time.

**Files:**
- Modify: `src/media/M2tsPacketizer.h`
- Modify: `src/media/LibavCaptureSource.cpp`

- [ ] **Step 1: Add mediaTimeUs to M2tsObject**

In `src/media/M2tsPacketizer.h`, change the struct:

```cpp
struct M2tsObject {
    QByteArray payload;
    std::uint64_t groupId = 0;
    std::uint64_t objectId = 0;
    // True when this object is the first of a new MOQT group (begins at a
    // random-access point). Always false for the file-source path.
    bool startsGroup = false;
};
```

to:

```cpp
struct M2tsObject {
    QByteArray payload;
    std::uint64_t groupId = 0;
    std::uint64_t objectId = 0;
    // True when this object is the first of a new MOQT group (begins at a
    // random-access point). Always false for the file-source path.
    bool startsGroup = false;
    // Media time (microseconds, capture-epoch relative) of the most recent video
    // frame whose bytes are in this object. 0 for the file-source path.
    std::uint64_t mediaTimeUs = 0;
};
```

- [ ] **Step 2: Populate offsetMediaUs for each video packet**

In `sendEncoderFrame` (`src/media/LibavCaptureSource.cpp`), the keyframe block ends at the closing brace of `if (isVideoKey) { ... }` (line ~793), immediately before `rc = av_interleaved_write_frame(outputFormat, packet.get());`. Insert, between that closing brace and the `rc = av_interleaved_write_frame(...)` line:

```cpp
            // Record this video packet's media time against the current muxed
            // byte offset so readObject can tag objects with a real media time
            // for egress pacing. Kept monotonic in both offset and media time.
            if (stream->video) {
                const std::uint64_t voffset = muxedConsumed + static_cast<std::uint64_t>(muxedBytes.size());
                std::uint64_t vmediaUs = offsetMediaUs.empty() ? 0 : offsetMediaUs.back().second;
                if (packet->pts != AV_NOPTS_VALUE) {
                    const std::int64_t us = av_rescale_q(packet->pts, stream->outputStream->time_base, AVRational{1, 1000000});
                    if (us > 0 && static_cast<std::uint64_t>(us) > vmediaUs) {
                        vmediaUs = static_cast<std::uint64_t>(us);
                    }
                }
                if (offsetMediaUs.empty() || voffset >= offsetMediaUs.back().first) {
                    offsetMediaUs.emplace_back(voffset, vmediaUs);
                }
            }
```

- [ ] **Step 3: Tag each object in readObject**

In `readObject`, after `muxedConsumed += static_cast<std::uint64_t>(alignedBytes);` (line ~658), insert:

```cpp
        // Advance the offset->mediaUs map to this object's end, remembering the
        // media time of the last video packet at or before it, and tag the
        // object so the pipeline can pace its release on real media time.
        const std::uint64_t mediaEnd = muxedConsumed;
        while (!offsetMediaUs.empty() && offsetMediaUs.front().first <= mediaEnd) {
            lastVideoMediaUs = offsetMediaUs.front().second;
            offsetMediaUs.pop_front();
        }
        object->mediaTimeUs = lastVideoMediaUs;
```

- [ ] **Step 4: Build via Docker bookworm**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...`, exit 0.

- [ ] **Step 5: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/M2tsPacketizer.h src/media/LibavCaptureSource.cpp
git commit -m "Tag capture objects with real media time from video PTS"
```
Commit message EXACTLY that, NO Claude tagline/Co-Authored-By.

---

## Task 4: Pace object release in the pipeline

Use the object's real media time and gate its release; add the config flag.

**Files:**
- Modify: `src/app/PublishConfig.h`
- Modify: `src/media/LivePipeline.cpp`

- [ ] **Step 1: Add the paceEgress config flag**

In `src/app/PublishConfig.h`, add near the other capture/encode fields (e.g. after `int keyframeIntervalMs = 1000;`):

```cpp
    bool paceEgress = true; // pace capture object release on real media time
```

- [ ] **Step 2: Include the pacing header + thread in LivePipeline.cpp**

At the top of `src/media/LivePipeline.cpp`, add to the includes (after `#include <cstdio>`):

```cpp
#include <thread>
```

and after the existing project includes (with `#include "LibavCaptureSource.h"` / `#include "MsftsMuxer.h"`):

```cpp
#include "EgressPacing.h"
```

- [ ] **Step 3: Add a pacing slack constant**

In the anonymous namespace near the top of `src/media/LivePipeline.cpp` (where `nowUnixUs()` is defined), add:

```cpp
// Latency slack (us) before an object is considered due. Small so steady-state
// latency stays near zero; only objects arriving ahead of their media time wait.
constexpr std::int64_t kPaceSlackUs = 8000;
```

- [ ] **Step 4: Use the real media time and pace release in the capture nextObject**

In the capture branch of `runLoop`, the `nextObject` lambda currently computes a synthetic media time and returns `published`. Replace this block (lines ~215-216):

```cpp
            published.mediaTimeUs = static_cast<std::uint64_t>(objects) * static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
            published.mediaDurationUs = static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
```

with:

```cpp
            published.mediaTimeUs = object.mediaTimeUs;
            published.mediaDurationUs = static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
```

Then, immediately before `return published;` at the end of the lambda (after `emit stats(objects, bytes);`), insert the pacing wait:

```cpp
            if (m_config.paceEgress) {
                if (pacingStartUs < 0) {
                    pacingStartUs = nowSteadyUs();
                }
                while (m_running.load(std::memory_order_acquire)) {
                    const std::int64_t delay = paceDelayUs(static_cast<std::int64_t>(published.mediaTimeUs),
                                                           nowSteadyUs() - pacingStartUs, kPaceSlackUs);
                    if (delay <= 0) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::int64_t>(delay, 5000)));
                }
            }
```

- [ ] **Step 5: Add the pacingStartUs local captured by the lambda**

The `nextObject` lambda captures by `[this, ...]` plus several locals. Add a pacing-start local just before the lambda definition (alongside `int64_t objects = 0; int64_t bytes = 0;` etc.) and capture it by reference:

Find (around line 184-189):

```cpp
        int64_t objects = 0;
        int64_t bytes = 0;
        std::uint64_t timelineObjectId = 0;
        std::optional<PublishedObject> pendingTimeline;
        const int timelineEveryObjects = std::max(1, 1000 / std::max(1, m_config.fragmentDurationMs));
        auto nextObject = [this, &capture, packetsPerObject, trackName, timelineTrackName, timelineEveryObjects, &objects, &bytes, &timelineObjectId, &pendingTimeline]() -> std::optional<PublishedObject> {
```

Replace with (add `pacingStartUs` local + capture):

```cpp
        int64_t objects = 0;
        int64_t bytes = 0;
        std::uint64_t timelineObjectId = 0;
        std::int64_t pacingStartUs = -1;
        std::optional<PublishedObject> pendingTimeline;
        const int timelineEveryObjects = std::max(1, 1000 / std::max(1, m_config.fragmentDurationMs));
        auto nextObject = [this, &capture, packetsPerObject, trackName, timelineTrackName, timelineEveryObjects, &objects, &bytes, &timelineObjectId, &pacingStartUs, &pendingTimeline]() -> std::optional<PublishedObject> {
```

- [ ] **Step 6: Build via Docker bookworm**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...`, exit 0.

- [ ] **Step 7: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/app/PublishConfig.h src/media/LivePipeline.cpp
git commit -m "Pace capture object release on real media time"
```
Commit message EXACTLY that, NO Claude tagline/Co-Authored-By.

---

## Task 5: Structural guards

**Files:**
- Modify: `tests/libav-capture-source-guards.sh`

- [ ] **Step 1: Append pacing/timeline assertions**

In `tests/libav-capture-source-guards.sh`, before the final `printf` success line, add:

```bash
# Audio and video PTS must share one capture epoch (A/V timeline).
if ! grep -q 'captureEpochUs' "$SOURCE"; then
  printf 'capture must stamp PTS from a shared capture epoch\n' >&2
  exit 1
fi
if ! grep -q 'ensureCaptureEpoch' "$SOURCE"; then
  printf 'capture must set the shared epoch once via ensureCaptureEpoch\n' >&2
  exit 1
fi

# Audio PTS must be epoch-anchored, not a bare from-zero counter.
if ! grep -q 'audioAnchored' "$SOURCE"; then
  printf 'audio PTS must anchor to the shared capture epoch\n' >&2
  exit 1
fi

# Objects must carry a real media time for pacing (offset->mediaUs map).
if ! grep -q 'offsetMediaUs' "$SOURCE"; then
  printf 'capture must record video media time per byte offset for pacing\n' >&2
  exit 1
fi
if ! grep -q 'object->mediaTimeUs = lastVideoMediaUs' "$SOURCE"; then
  printf 'readObject must tag objects with the real video media time\n' >&2
  exit 1
fi
```

Also add a guard to `tests/libav-capture-source-guards.sh` for the pipeline pacing wait by pointing at LivePipeline (define a new var near the top where `SOURCE` is set):

Find the variable definitions near the top (where `SOURCE="$REPO_ROOT/src/media/LibavCaptureSource.cpp"` is set) and add after it:

```bash
PIPELINE="$REPO_ROOT/src/media/LivePipeline.cpp"
```

Then add (also before the final success printf):

```bash
# The pacing wait must be stop-checkable (bounded), not an unbounded sleep.
if ! grep -q 'paceDelayUs(' "$PIPELINE"; then
  printf 'pipeline must pace object release via paceDelayUs\n' >&2
  exit 1
fi
if ! grep -q 'm_running.load' "$PIPELINE"; then
  printf 'pipeline pacing wait must re-check m_running each step\n' >&2
  exit 1
fi
```

- [ ] **Step 2: Run the guard and the full suite**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash tests/libav-capture-source-guards.sh; echo "rc=$?"
for t in tests/*.sh; do
  out=$(bash "$t" 2>&1); rc=$?
  printf "%-50s %s\n" "$(basename "$t")" "$([ $rc -eq 0 ] && echo PASS || echo "FAIL(rc=$rc)")"
done
```
Expected: `libav-capture-source-guards.sh` prints its pass line (rc=0); every script PASS.

- [ ] **Step 3: Run the pacing unit test via CTest wiring (sanity)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
g++ -std=c++20 -I src tests/pace_delay_test.cpp -o /tmp/pace_test && /tmp/pace_test; echo "test_exit=$?"
```
Expected: `test_exit=0`.

- [ ] **Step 4: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add tests/libav-capture-source-guards.sh
git commit -m "Guard the A/V timeline and egress pacing invariants"
```
Commit message EXACTLY that, NO Claude tagline/Co-Authored-By.

---

## Task 6: Verification — A/V timeline + smooth cadence

**Files:** none (capture-to-file demuxer check + manual run; user-performed for the live relay run).

- [ ] **Step 1: Independent A/V-timeline check (capture our exact mux to a file)**

This proves audio and video share a timeline. It uses the same libav mjpeg->h264->aac->mpegts path the app uses. Requires a mic at `hw:0` or similar; if no audio device is available, skip and rely on the live run's red5.log.

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
timeout 8 ffmpeg -hide_banner -loglevel error \
  -f v4l2 -input_format mjpeg -video_size 1920x1080 -framerate 30 -i /dev/video2 \
  -f alsa -i default \
  -c:v libx264 -preset ultrafast -b:v 2500k -g 30 -pix_fmt yuv420p \
  -c:a aac -ar 48000 \
  -f mpegts /tmp/avts.ts -t 3 2>&1 | tail -3
echo "rc=$?; size:"; ls -l /tmp/avts.ts 2>/dev/null
echo "=== first video PTS vs first audio PTS (should be close; PCR on video PID) ==="
ffprobe -hide_banner -v error -show_entries packet=stream_index,codec_type,pts_time -of csv /tmp/avts.ts 2>/dev/null | awk -F, 'BEGIN{v="";a=""} /video/ && v==""{v=$3} /audio/ && a==""{a=$3} END{print "first video pts_time="v"  first audio pts_time="a}'
```
Expected: first video and first audio `pts_time` within a small delta (shared epoch). NOTE: this `ffmpeg` capture uses ffmpeg's own muxer, which already shares a clock; it is a sanity check of the tooling/expectation. The authoritative check is the app's own output in the live run (Step 2). If the app build is run with an `MOQ2TS_DUMP_TS` path or the muxed bytes are otherwise captured, ffprobe that file instead for the true app-side proof.

- [ ] **Step 2: Live relay run (ask the user to perform this)**

Publish a camera+mic stream to the relay for ~15s, then stop. Capture the relay `red5.log`.

Success criteria:
- Object-arrival cadence is smooth: program-1 objects arrive spread across each GOP interval, NOT in tight bursts of ~8 within ~20ms followed by ~1.5s idle (directly comparable to the pre-fix pattern). Check with:
  ```bash
  LC_ALL=C grep -aE "Processing object for track: program-1 " ~/Downloads/red5.log | grep -av timeline | sed -E 's/^.*([0-9]{2}:[0-9]{2}:[0-9]{2},[0-9]{3}).*/\1/' | head -40
  ```
  Expect roughly even spacing (~33ms at 30fps per object-group fraction), not clustered.
- Playback is no longer choppy and A/V are in sync.
- No `[moqxr][stop]` join-timeout and clean stop (pacing wait is bounded).

- [ ] **Step 3: Report results**

Summarize the observed object cadence (clustered vs even), playback smoothness, and A/V sync. If still bursty, capture the publisher stderr and red5.log so the offset->mediaUs tagging and pacing wait can be inspected. NOTE: the relay-side TSHandler ingest stall (separate red5pro-plugins issue) can independently affect playback; pacing fixes only the publisher-side burst.

---

## Notes for the implementer

- Single repo (moq2ts); commit all tasks here. moqxr untouched.
- Do NOT add Claude author taglines / "Generated with" / "Co-Authored-By" to commits.
- `paceDelayUs`/`nowSteadyUs` are pure and header-only (`EgressPacing.h`); keep them free of Qt/libav so the unit test links only that header.
- The capture epoch is steady-clock; both the epoch and the pipeline's `pacingStartUs` read `nowSteadyUs()`, and an object's `mediaTimeUs` is epoch-relative, so all three are on one comparable clock.
- The pacing wait MUST stay bounded and `m_running`-checked (<=5ms steps) so the previously-implemented bounded-stop behavior is preserved.
- Do not change the file/M2TS source path; its objects keep `mediaTimeUs = 0` and are not paced by this work.
- `/tmp/avts.ts` and `/tmp/pace_test` are throwaway; do not commit them.
