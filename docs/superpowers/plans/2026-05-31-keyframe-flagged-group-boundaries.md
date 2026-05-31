# Keyframe-flagged group boundaries Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect MOQT group boundaries from the encoder's authoritative `AV_PKT_FLAG_KEY` (flushing the muxer at each keyframe for a byte-exact offset), replacing the non-functional TS `random_access_indicator` byte-scanner.

**Architecture:** In `sendEncoderFrame`, when a video packet has `AV_PKT_FLAG_KEY`, flush the interleaver, record the current produced-byte offset as a group boundary (first IDR excepted), then write the keyframe. `readObject` is unchanged. Delete `scanForRapBoundaries` and its `scannedTo`/`videoPidValue` plumbing.

**Tech Stack:** C++20, Qt 6, FFmpeg/libav, Docker bookworm build. Tests = bash source guards + container build. IGNORE post-build packaging noise after "Build complete" (`ldd: libavfilter.so.8`, `rm: cannot remove`); only `*.cpp:line:col: error:` counts as failure. NOTE: this harness sometimes reports shifting line numbers — every edit below uses an exact text anchor, not a line number. Read each region immediately before editing it.

**Reference:** `docs/superpowers/specs/2026-05-31-keyframe-flagged-group-boundaries-design.md`.

**Base:** branch `main` at `f2b27bb`. All code edits in `src/media/LibavCaptureSource.cpp`; guard in `tests/keyframe-aligned-groups-guards.sh`.

---

## Task 1: Record group boundaries on AV_PKT_FLAG_KEY in sendEncoderFrame

**Files:** `src/media/LibavCaptureSource.cpp`

- [ ] **Step 1: Replace the write block**

Find this exact block inside `sendEncoderFrame`:
```cpp
            av_packet_rescale_ts(packet.get(), stream->encoder->time_base, stream->outputStream->time_base);
            packet->stream_index = stream->outputStream->index;
            rc = av_interleaved_write_frame(outputFormat, packet.get());
            if (rc < 0) {
                if (error) {
                    *error = QStringLiteral("Failed writing MPEG-TS packet: %1").arg(avError(rc));
                }
                return false;
            }
```
Replace with:
```cpp
            av_packet_rescale_ts(packet.get(), stream->encoder->time_base, stream->outputStream->time_base);
            packet->stream_index = stream->outputStream->index;
            // Use the encoder's authoritative keyframe flag to mark MOQT group
            // boundaries. Flush the interleaver first so all previously queued
            // packets land in muxedBytes; the byte offset is then the exact start
            // of this keyframe's TS packets (group-aligned random access).
            const bool isVideoKey = stream->video && (packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (isVideoKey) {
                av_interleaved_write_frame(outputFormat, nullptr);
                const std::uint64_t offset = muxedConsumed + static_cast<std::uint64_t>(muxedBytes.size());
                if (!sawVideoKeyframe) {
                    // First IDR: group 0 starts at byte 0 and contains it; no push.
                    sawVideoKeyframe = true;
                    std::fprintf(stderr, "[moqxr][diag] keyframe groups: first IDR at offset=%llu\n",
                                 (unsigned long long)offset);
                } else {
                    groupBoundaries.push_back(offset);
                    ++rapBoundaryCount;
                    if (rapBoundaryCount % 30 == 1) {
                        std::fprintf(stderr,
                                     "[moqxr][diag] keyframe boundaries=%llu latest offset=%llu\n",
                                     (unsigned long long)rapBoundaryCount,
                                     (unsigned long long)offset);
                    }
                }
            }
            rc = av_interleaved_write_frame(outputFormat, packet.get());
            if (rc < 0) {
                if (error) {
                    *error = QStringLiteral("Failed writing MPEG-TS packet: %1").arg(avError(rc));
                }
                return false;
            }
```
(`<cstdio>`, `groupBoundaries`, `sawVideoKeyframe`, `rapBoundaryCount`, `muxedConsumed` all already exist. `av_interleaved_write_frame(ctx, nullptr)` flushes the queue and returns >=0 on success; its return is not assigned to `rc` because the subsequent real write's return is what matters for error handling.)

- [ ] **Step 2: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Built target moq2ts-publisher` / "Build complete", zero `*.cpp:...: error:` lines.

- [ ] **Step 3: Commit**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Record group boundaries from encoder keyframe flag"
```

---

## Task 2: Remove the dead RAP byte-scanner

**Files:** `src/media/LibavCaptureSource.cpp`

- [ ] **Step 1: Remove the scanner call in pumpOnce**

Find (the tail of `pumpOnce`):
```cpp
        }
        scanForRapBoundaries();
        return true;
    }
```
Replace with:
```cpp
        }
        return true;
    }
```
(There is exactly one `scanForRapBoundaries();` call — verify with `grep -c 'scanForRapBoundaries();' src/media/LibavCaptureSource.cpp` returns 1 before editing.)

- [ ] **Step 2: Delete the scanner method**

Delete the entire method, from its doc comment through its closing brace. It begins with:
```cpp
    // Scans newly-landed, packet-aligned TS bytes for video random-access points
    // and records each (except the first, which group 0 already contains) as a
    // group boundary at its exact absolute byte offset.
    void scanForRapBoundaries() {
```
and ends at the matching `    }` (the method whose body computes `videoPid`, scans 188-byte packets, tests `random_access_indicator`, pushes `groupBoundaries`, and sets `scannedTo`). Remove the whole method. (Read the method first to capture its exact text for the deletion; it is self-contained and nothing else calls it after Step 1.)

- [ ] **Step 2b: Verify the diag strings moved, not duplicated**

After deletion, `grep -c 'moqxr\]\[diag\]' src/media/LibavCaptureSource.cpp` must be `2` (the two new lines in `sendEncoderFrame` from Task 1) — the scanner's old diag lines are gone with it.

- [ ] **Step 3: Remove the videoPidValue capture block**

Find (after `headerWritten = true;`):
```cpp
        headerWritten = true;

        for (const auto& s : streams) {
            if (s->video && s->outputStream) {
                videoPidValue = s->outputStream->id;
                break;
            }
        }

        extractInitData(muxedBytes, &initDataBytes, &pmtPidValue, &pcrPidValue);
```
Replace with:
```cpp
        headerWritten = true;

        extractInitData(muxedBytes, &initDataBytes, &pmtPidValue, &pcrPidValue);
```

- [ ] **Step 4: Remove the now-unused members**

Find:
```cpp
    std::uint64_t muxedConsumed = 0;
    // Absolute offset up to which muxedBytes has been scanned for random-access
    // points. Boundaries are detected by scanning landed bytes, not at encode
    // time, so they are exact regardless of A/V interleaving.
    std::uint64_t scannedTo = 0;
    // TS PID carrying the video elementary stream, assigned by the muxer at
    // write-header time; -1 until known (falls back to pcrPidValue in the scan).
    int videoPidValue = -1;
    std::uint64_t nextGroupId = 0;
```
Replace with:
```cpp
    std::uint64_t muxedConsumed = 0;
    std::uint64_t nextGroupId = 0;
```
(`scannedTo` and `videoPidValue` are only referenced by the deleted scanner; removing them now makes the build prove there are no stragglers.)

- [ ] **Step 5: Verify no dangling references**

Run:
```bash
grep -n 'scanForRapBoundaries\|scannedTo\|videoPidValue\|random_access_indicator' src/media/LibavCaptureSource.cpp
```
Expected: NO output (all gone).

- [ ] **Step 6: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Built target moq2ts-publisher` / "Build complete", zero real compile errors. (A leftover reference to a removed member would fail here.)

- [ ] **Step 7: Confirm diag strings in the binary**

Run: `strings build-bookworm/install/bin/moq2ts-publisher | grep -c 'moqxr\]\[diag\] keyframe'`
Expected: `2`.

- [ ] **Step 8: Commit**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Remove dead TS random-access-indicator byte scanner"
```

---

## Task 3: Update the source guard

**Files:** `tests/keyframe-aligned-groups-guards.sh`

- [ ] **Step 1: Swap scanner assertions for keyframe-flag assertions**

The guard currently contains these three assertions (added for the byte-scanner):
```bash
grep -q 'scanForRapBoundaries' "$CAPTURE" \
  || fail "capture must scan muxed TS for random-access points"

grep -q 'random_access_indicator' "$CAPTURE" \
  || fail "RAP scan must test the TS random_access_indicator"

grep -q 'videoPidValue' "$CAPTURE" \
  || fail "capture must track the muxer-assigned video PID for RAP scanning"
```
Replace all three with:
```bash
grep -q 'AV_PKT_FLAG_KEY' "$CAPTURE" \
  || fail "capture must detect keyframes via AV_PKT_FLAG_KEY"

grep -q 'groupBoundaries.push_back' "$CAPTURE" \
  || fail "capture must record a group boundary at each keyframe"
```
Leave all other assertions unchanged (`bool startsGroup`, `groupBoundaries`, `object->startsGroup = true;`, `randomAccessActive` in CAPTURE + HDR, `if (catalog.randomAccess)`, `keyframeIntervalMs`, `gop_size = gopSize`).

- [ ] **Step 2: Run the guard**

Run: `bash tests/keyframe-aligned-groups-guards.sh`
Expected: `keyframe-aligned-groups guards passed`, exit 0. If a grep fails, fix ONLY the test pattern to match the committed source.

- [ ] **Step 3: Full guard suite**

Run: `for t in tests/*.sh; do echo "== $t =="; bash "$t"; echo "rc=$?"; done`
Expected: all 11 scripts exit 0.

- [ ] **Step 4: Commit**

```bash
git add tests/keyframe-aligned-groups-guards.sh
git commit -m "Guard keyframe-flag group-boundary detection"
```

---

## Verification (manual, after all tasks)

Run a capture publish capturing PUBLISHER stderr:
```bash
build-bookworm/install/moq2ts-publisher-bookworm 2>&1 | tee /tmp/pub.log
```
- Success: `/tmp/pub.log` shows `[moqxr][diag] keyframe groups: first IDR at offset=...`
  then `keyframe boundaries=N latest offset=...` climbing; relay log shows objects in
  groups beyond 0 (`program-1/1/...`, `program-1/2/...`); stream becomes playable.
- If the first-IDR line appears but no boundaries climb → keyframes only fire once
  (GOP not applied) — unlikely given prior `frame I:9..13`. If neither line appears →
  `sendEncoderFrame` not reached / not the video stream — investigate `stream->video`.

---

## Self-Review notes

- Spec coverage: keyframe-flag boundary + flush + diag → Task 1; remove scanner +
  scannedTo/videoPidValue + pumpOnce call + PID capture → Task 2; guard → Task 3.
  `readObject` intentionally unchanged.
- Type consistency: `offset` (uint64), reuses `groupBoundaries` (deque<uint64>),
  `sawVideoKeyframe` (bool), `rapBoundaryCount` (uint64), `muxedConsumed` (uint64) —
  all already declared. `AV_PKT_FLAG_KEY` from libavcodec (already included).
- Flush return intentionally unchecked (design); the real write's `rc` drives errors.
- Out of scope (idle close, queue overflow, catalog cold-start m2tsRandomAccess)
  recorded in the design, untouched.
