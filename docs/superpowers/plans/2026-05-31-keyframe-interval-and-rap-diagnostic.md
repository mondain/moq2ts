# Configurable keyframe interval + RAP diagnostic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce a bounded, configurable H.264 IDR cadence on the capture encoder so periodic keyframes (and thus MOQT group boundaries) exist, and add a throttled publisher-side diagnostic that proves whether the RAP scanner is finding boundaries.

**Architecture:** Add `keyframeIntervalMs` to PublishConfig; in `LibavCaptureSource::addVideo`, set a closed GOP (`gop_size`, `max_b_frames=0`) plus non-fatal x264 keyint/scenecut opts before `avcodec_open2`. Add a counter + throttled stderr to `scanForRapBoundaries`.

**Tech Stack:** C++20, Qt 6, FFmpeg/libav (libopenh264 primary, libx264 fallback), Docker bookworm build. Tests = bash source-guard scripts + container build. IGNORE post-build packaging noise (`ldd: libavfilter.so.8`, `rm: cannot remove`) that appears AFTER "Build complete"; only `*.cpp:line:col: error:` diagnostics count as failure.

**Reference:** `docs/superpowers/specs/2026-05-31-keyframe-interval-and-rap-diagnostic-design.md`.

**Base:** branch `main` at `d474af6`.

---

## Task 1: Add keyframeIntervalMs to PublishConfig

**Files:** Modify `src/app/PublishConfig.h`

- [ ] **Step 1: Add the field**

In `src/app/PublishConfig.h`, the current lines are:
```cpp
    int videoFramerate = 30;
    int videoTargetBitrateKbps = 2500;
```
Change to:
```cpp
    int videoFramerate = 30;
    int videoTargetBitrateKbps = 2500;
    int keyframeIntervalMs = 1000;  // live H.264 IDR cadence; MOQT group cadence follows it
```

- [ ] **Step 2: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `[100%] Built target moq2ts-publisher` / "Build complete", zero `*.cpp:...: error:` lines. (A new defaulted field is source-compatible.)

- [ ] **Step 3: Commit**

```bash
git add src/app/PublishConfig.h
git commit -m "Add configurable keyframeIntervalMs to PublishConfig"
```

---

## Task 2: Enforce closed GOP / IDR cadence in the encoder

**Files:** Modify `src/media/LibavCaptureSource.cpp` (`addVideo`, encoder setup before `avcodec_open2`)

- [ ] **Step 1: Set GOP and closed-GOP + non-fatal x264 opts**

The current block is:
```cpp
        stream->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        stream->encoder->bit_rate = static_cast<int64_t>(config.videoTargetBitrateKbps) * 1000;
        av_opt_set(stream->encoder->priv_data, "profile", "baseline", 0);
        int rc = avcodec_open2(stream->encoder, encoder, nullptr);
```
Replace it with:
```cpp
        stream->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        stream->encoder->bit_rate = static_cast<int64_t>(config.videoTargetBitrateKbps) * 1000;
        // Bounded, closed GOP so the live stream emits periodic IDRs (every
        // keyframeIntervalMs). Without this the encoder uses an effectively long
        // default GOP, no random-access points appear after the first frame, and
        // MOQT grouping degenerates to a single group.
        const int frameRate = std::max(1, config.videoFramerate);
        int gopSize = static_cast<int>(
            (static_cast<long long>(frameRate) * std::max(1, config.keyframeIntervalMs) + 500) / 1000);
        gopSize = std::max(1, gopSize);
        stream->encoder->gop_size = gopSize;
        stream->encoder->max_b_frames = 0;  // closed, low-latency GOP
        av_opt_set(stream->encoder->priv_data, "profile", "baseline", 0);
        // Belt-and-suspenders for the libx264 fallback: pin keyint and disable
        // scene-cut so IDRs land on a fixed period. These options do not exist on
        // libopenh264 (the primary encoder), which honors gop_size directly; the
        // return codes are intentionally ignored so an absent option is not fatal.
        av_opt_set_int(stream->encoder->priv_data, "keyint", gopSize, 0);
        av_opt_set_int(stream->encoder->priv_data, "min-keyint", gopSize, 0);
        av_opt_set(stream->encoder->priv_data, "sc_threshold", "0", 0);
        int rc = avcodec_open2(stream->encoder, encoder, nullptr);
```
Note: `std::max` / `std::min` are already used in this file (`<algorithm>` is included). The `av_opt_set*` return values are deliberately not checked — on libopenh264 they return an error for the unknown x264 options and that is expected and harmless.

- [ ] **Step 2: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Built target moq2ts-publisher` / "Build complete", zero real compile errors.

- [ ] **Step 3: Commit**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Set closed GOP with configurable IDR interval on capture encoder"
```

---

## Task 3: Add throttled RAP-scan diagnostic

**Files:** Modify `src/media/LibavCaptureSource.cpp` (include, Impl member, `scanForRapBoundaries`)

- [ ] **Step 1: Include <cstdio>**

The current include block ends with:
```cpp
#include <memory>
#include <vector>
```
Change to:
```cpp
#include <cstdio>
#include <memory>
#include <vector>
```
(Keep alphabetical-ish ordering with the existing `<algorithm>`/`<cmath>`/`<deque>`/`<functional>` group; placing `<cstdio>` before `<memory>` is fine.)

- [ ] **Step 2: Add a boundary counter member**

The current Impl members include:
```cpp
    std::uint64_t nextObjectIdInGroup = 0;
    std::uint64_t pendingGroupBoundary = 0;  // absolute offset of the next group start, or 0 if none
```
Add after them:
```cpp
    std::uint64_t rapBoundaryCount = 0;  // diagnostic: number of RAP boundaries recorded
```

- [ ] **Step 3: Log the PID at first RAP and throttle a running count**

In `scanForRapBoundaries`, the current first-RAP / push branch is:
```cpp
            if (!sawVideoKeyframe) {
                // First RAP: group 0 starts at byte 0 and contains it; do not push.
                sawVideoKeyframe = true;
            } else {
                groupBoundaries.push_back(pos);
            }
```
Replace it with:
```cpp
            if (!sawVideoKeyframe) {
                // First RAP: group 0 starts at byte 0 and contains it; do not push.
                sawVideoKeyframe = true;
                std::fprintf(stderr, "[moqxr][diag] RAP scan: videoPid=%d pcrPid=%d\n",
                             videoPid, pcrPidValue);
            } else {
                groupBoundaries.push_back(pos);
                ++rapBoundaryCount;
                if (rapBoundaryCount % 30 == 1) {
                    std::fprintf(stderr,
                                 "[moqxr][diag] RAP boundaries=%llu latest offset=%llu\n",
                                 (unsigned long long)rapBoundaryCount,
                                 (unsigned long long)pos);
                }
            }
```
(`% 30 == 1` logs the 1st, 31st, 61st, … boundary — first hit logs immediately, then every 30.)

- [ ] **Step 4: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Built target moq2ts-publisher` / "Build complete", zero real compile errors.

- [ ] **Step 5: Confirm the diag strings are compiled into the binary**

Run:
```bash
strings build-bookworm/install/bin/moq2ts-publisher | grep -c 'moqxr\]\[diag\] RAP'
```
Expected: `2` (the two new format strings).

- [ ] **Step 6: Commit**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Add throttled RAP-scan diagnostic to capture source"
```

---

## Task 4: Guard for the keyframe-interval wiring

**Files:** Modify `tests/keyframe-aligned-groups-guards.sh`

- [ ] **Step 1: Add assertions**

The guard currently ends with:
```bash
grep -q 'if (catalog.randomAccess)' "$MUXER" \
  || fail "m2tsRandomAccess must be gated on catalog.randomAccess"

printf 'keyframe-aligned-groups guards passed\n'
```
Insert before the `printf`:
```bash
grep -q 'keyframeIntervalMs' "$REPO_ROOT/src/app/PublishConfig.h" \
  || fail "PublishConfig must expose keyframeIntervalMs"

grep -q 'gop_size = gopSize' "$CAPTURE" \
  || fail "encoder must set a bounded gop_size for periodic IDRs"

```
(Keep the existing `printf 'keyframe-aligned-groups guards passed\n'` line after.)

- [ ] **Step 2: Run the guard**

Run: `bash tests/keyframe-aligned-groups-guards.sh`
Expected: `keyframe-aligned-groups guards passed`, exit 0. If a grep fails, the pattern doesn't match committed source — read the source and correct ONLY the test pattern.

- [ ] **Step 3: Full guard suite**

Run: `for t in tests/*.sh; do echo "== $t =="; bash "$t"; echo "rc=$?"; done`
Expected: every script exits 0 (the two formerly-failing guards were fixed in `14b2a1d`, so all 11 should pass).

- [ ] **Step 4: Commit**

```bash
git add tests/keyframe-aligned-groups-guards.sh
git commit -m "Guard the keyframe-interval GOP wiring"
```

---

## Verification (manual, after all tasks)

Rebuild is already covered per-task. To validate end-to-end:
1. Run a capture publish from a terminal, capturing the PUBLISHER stderr:
   `build-bookworm/install/moq2ts-publisher-bookworm 2>&1 | tee /tmp/pub.log`
2. In `/tmp/pub.log` expect `[moqxr][diag] RAP scan: videoPid=<pid> pcrPid=<pid>` then
   `[moqxr][diag] RAP boundaries=N latest offset=...` with N climbing.
3. In the relay log expect objects in groups beyond 0 (`program-1/1/...`,
   `program-1/2/...`) and a playable stream.
- If boundaries stay 0 with a sensible videoPid → the scanner's RAP test needs
  revisiting; if videoPid is wrong → the PID capture/fallback needs revisiting.
  The diagnostic distinguishes the two.

---

## Self-Review notes

- Spec coverage: config field → Task 1; closed GOP + IDR cadence + x264 opts →
  Task 2; `<cstdio>` + counter + throttled stderr → Task 3; guard → Task 4.
- Type consistency: `keyframeIntervalMs` (int), `gopSize` (int), `rapBoundaryCount`
  (uint64); `gop_size`/`max_b_frames` are AVCodecContext fields; `av_opt_set_int`
  for integer opts, `av_opt_set` for string `sc_threshold`.
- Non-fatal x264 opts: return codes intentionally unchecked (libopenh264 lacks
  them) — explicitly stated so the implementer does not assign them to `rc`.
- Out of scope (idle session close, catalog cold-start m2tsRandomAccess) recorded
  in the design, not touched here.
