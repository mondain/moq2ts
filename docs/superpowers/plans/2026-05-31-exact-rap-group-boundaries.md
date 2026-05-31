# Exact RAP-based group boundaries Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make each capture group boundary land exactly on a video random-access point by scanning the muxed TS byte stream (post-interleave) instead of recording encode-time submit offsets.

**Architecture:** Stop pushing boundaries in `sendEncoderFrame`. Instead, after bytes land in `muxedBytes` (in `pumpOnce`), scan packet-aligned 188-byte TS packets for the video PID's random_access_indicator and push the absolute offset of each RAP into the existing `groupBoundaries` deque. `readObject` (boundary-aware slicing + group/object IDs) is UNCHANGED.

**Tech Stack:** C++20, Qt 6, FFmpeg/libav, Docker bookworm build. Tests are bash source-guard scripts + the container build (compile/integration check). Ignore post-build packaging noise (`ldd: libavfilter.so.8`, `rm: cannot remove`) that appears AFTER "Build complete".

**Reference:** `docs/superpowers/specs/2026-05-31-exact-rap-group-boundaries-design.md`. Drafts: `docs/draft-gregoire-moq-msfts-00.txt` 5.2/5.3/6.8.

**Base:** branch `main` at `7771ecb`. All edits go in `src/media/LibavCaptureSource.cpp` (Tasks 1-3) and `tests/keyframe-aligned-groups-guards.sh` (Task 4).

---

## Task 1: Swap encode-time boundary recording for scan state

**Files:** Modify `src/media/LibavCaptureSource.cpp`

This task removes the imprecise encode-time machinery and adds the scan-state members. After it, boundaries are NOT recorded anywhere yet (Task 2 adds the scanner) — that's fine, it still compiles (grouping just won't fire until Task 2).

- [ ] **Step 1: Replace the muxedProduced member with scan-state members**

In the Impl member block, the current lines are:
```cpp
    // Total bytes ever appended to muxedBytes; the absolute cursor for the byte
    // currently at muxedBytes[0] is (muxedConsumed).
    std::uint64_t muxedProduced = 0;
    std::uint64_t muxedConsumed = 0;
```
Replace those four lines with:
```cpp
    std::uint64_t muxedConsumed = 0;
    // Absolute offset up to which muxedBytes has been scanned for random-access
    // points. Boundaries are detected by scanning landed bytes, not at encode
    // time, so they are exact regardless of A/V interleaving.
    std::uint64_t scannedTo = 0;
    // TS PID carrying the video elementary stream, assigned by the muxer at
    // write-header time; -1 until known (falls back to pcrPidValue in the scan).
    int videoPidValue = -1;
```
(`muxedConsumed`, `nextGroupId`, `sawVideoKeyframe`, `hasVideoStream`, `nextObjectIdInGroup`, `pendingGroupBoundary`, `groupBoundaries` stay as they are.)

- [ ] **Step 2: Remove the encode-time boundary block from sendEncoderFrame**

In `sendEncoderFrame`, the current write block is:
```cpp
            av_packet_rescale_ts(packet.get(), stream->encoder->time_base, stream->outputStream->time_base);
            packet->stream_index = stream->outputStream->index;
            const bool isVideoKey = stream->video && (packet->flags & AV_PKT_FLAG_KEY) != 0;
            const std::uint64_t beforeSize = muxedProduced;
            rc = av_interleaved_write_frame(outputFormat, packet.get());
            if (rc < 0) {
                if (error) {
                    *error = QStringLiteral("Failed writing MPEG-TS packet: %1").arg(avError(rc));
                }
                return false;
            }
            // Account for everything writePacket appended during this call.
            muxedProduced = muxedConsumed + static_cast<std::uint64_t>(muxedBytes.size());
            if (isVideoKey) {
                // The keyframe's TS packets begin at beforeSize. Record it as a
                // group boundary unless it coincides with the very first bytes
                // (the initial group needs no explicit boundary).
                if (beforeSize > 0 || !groupBoundaries.empty()) {
                    groupBoundaries.push_back(beforeSize);
                }
                sawVideoKeyframe = true;
            }
```
Replace it with (drop isVideoKey/beforeSize/muxedProduced/boundary push — keep only the write and its error check):
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

- [ ] **Step 3: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `[100%] Built target moq2ts-publisher` / "Build complete", zero `error:` lines. (`muxedProduced` is now gone and unreferenced; `AV_PKT_FLAG_KEY` no longer appears — both expected. `scannedTo`/`videoPidValue` are unused-but-defaulted members, which is fine.)

- [ ] **Step 4: Commit**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Drop encode-time keyframe boundary recording"
```

---

## Task 2: Capture the video PID and scan landed bytes for RAPs

**Files:** Modify `src/media/LibavCaptureSource.cpp`

- [ ] **Step 1: Capture the muxer-assigned video PID after write-header**

The current code right after the header write is:
```cpp
        rc = avformat_write_header(outputFormat, nullptr);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to write MPEG-TS header: %1").arg(avError(rc));
            }
            return false;
        }
        headerWritten = true;
```
Immediately after `headerWritten = true;`, add:
```cpp
        for (const auto& s : streams) {
            if (s->video && s->outputStream) {
                videoPidValue = s->outputStream->id;
                break;
            }
        }
```
(The mpegts muxer assigns each output stream's TS PID into `AVStream::id` during `avformat_write_header`.)

- [ ] **Step 2: Add the RAP scan helper**

Add this method to Impl, placed immediately before `bool pumpOnce(QString* error) {`:
```cpp
    // Scans newly-landed, packet-aligned TS bytes for video random-access points
    // and records each (except the first, which group 0 already contains) as a
    // group boundary at its exact absolute byte offset.
    void scanForRapBoundaries() {
        const int videoPid = videoPidValue >= 0 ? videoPidValue : pcrPidValue;
        if (videoPid < 0) {
            return;
        }
        const std::uint64_t producedEnd = muxedConsumed + static_cast<std::uint64_t>(muxedBytes.size());
        std::uint64_t pos = std::max(scannedTo, muxedConsumed);
        // Align pos to a packet boundary relative to the current buffer start.
        const std::uint64_t bufBase = muxedConsumed;
        if (pos > bufBase) {
            const std::uint64_t rel = pos - bufBase;
            pos = bufBase + (rel / 188) * 188;
        }
        const char* data = muxedBytes.constData();
        for (; pos + 188 <= producedEnd; pos += 188) {
            const int idx = static_cast<int>(pos - bufBase);
            const unsigned char* pkt = reinterpret_cast<const unsigned char*>(data + idx);
            if (pkt[0] != 0x47) {
                continue;  // not packet-aligned / corrupt; skip this 188 window
            }
            const int pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
            if (pid != videoPid) {
                continue;
            }
            const bool pusi = (pkt[1] & 0x40) != 0;
            const int adaptation = (pkt[3] >> 4) & 0x3;  // 2 or 3 => adaptation present
            if (!pusi || (adaptation != 2 && adaptation != 3)) {
                continue;
            }
            const int adaptationLen = pkt[4];
            if (adaptationLen <= 0) {
                continue;
            }
            const bool randomAccess = (pkt[5] & 0x40) != 0;  // random_access_indicator
            if (!randomAccess) {
                continue;
            }
            if (!sawVideoKeyframe) {
                // First RAP: group 0 starts at byte 0 and contains it; do not push.
                sawVideoKeyframe = true;
            } else {
                groupBoundaries.push_back(pos);
            }
        }
        scannedTo = pos > scannedTo ? pos : scannedTo;
    }
```

- [ ] **Step 3: Call the scanner after bytes land in pumpOnce**

The end of `pumpOnce` currently is:
```cpp
                if (!encodeFrame(stream.get(), frame.get(), error)) {
                    return false;
                }
            }
        }
        return true;
    }
```
Change the final `return true;` of `pumpOnce` to scan first:
```cpp
                if (!encodeFrame(stream.get(), frame.get(), error)) {
                    return false;
                }
            }
        }
        scanForRapBoundaries();
        return true;
    }
```
(Only the `pumpOnce` return — do not alter the identical `return true;` lines in other methods.)

- [ ] **Step 4: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Built target moq2ts-publisher` / "Build complete", zero `error:` lines.

- [ ] **Step 5: Commit**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Detect group boundaries by scanning muxed TS for video RAPs"
```

---

## Task 3: Guard against rescanning already-consumed bytes (correctness check)

**Files:** Modify `src/media/LibavCaptureSource.cpp` (only if needed — verification task)

The scanner advances `scannedTo` and starts at `max(scannedTo, muxedConsumed)`, so consumed bytes are never rescanned and a byte is scanned at most once. This task verifies that invariant by reading the code; no edit is expected unless a defect is found.

- [ ] **Step 1: Confirm the scan window never regresses or double-counts**

Read `scanForRapBoundaries` and confirm:
- `pos` starts at `max(scannedTo, muxedConsumed)` so it never revisits consumed bytes after `readObject` advances `muxedConsumed`.
- `scannedTo` only increases (`pos > scannedTo ? pos : scannedTo`).
- Boundaries pushed are absolute offsets `>= muxedConsumed`, which `readObject`'s `recomputeTarget` already prunes (`<= windowStart`).
- The first RAP is skipped (no push) and sets `sawVideoKeyframe`.

If all hold, no code change. If any fails, fix minimally and note it.

- [ ] **Step 2: Verify build (no-op rebuild ok)**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Built target moq2ts-publisher`, zero `error:` lines.

- [ ] **Step 3: Commit only if a fix was made**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Harden RAP scan window bookkeeping"
```
If no change was needed, skip the commit and record "no change required".

---

## Task 4: Update the source guard test

**Files:** Modify `tests/keyframe-aligned-groups-guards.sh`

The guard currently asserts `AV_PKT_FLAG_KEY` appears in the capture source. That token is removed by Task 1, so the assertion must change to the scan-based invariants.

- [ ] **Step 1: Replace the AV_PKT_FLAG_KEY assertion**

In `tests/keyframe-aligned-groups-guards.sh`, the current block is:
```bash
grep -q 'AV_PKT_FLAG_KEY' "$CAPTURE" \
  || fail "capture must detect keyframes via AV_PKT_FLAG_KEY to mark group boundaries"
```
Replace it with:
```bash
grep -q 'scanForRapBoundaries' "$CAPTURE" \
  || fail "capture must scan muxed TS for random-access points"

grep -q 'random_access_indicator' "$CAPTURE" \
  || fail "RAP scan must test the TS random_access_indicator"

grep -q 'videoPidValue' "$CAPTURE" \
  || fail "capture must track the muxer-assigned video PID for RAP scanning"
```
(The comment `// random_access_indicator` in Task 2's scanner satisfies the second grep. Keep all other assertions in the file unchanged: startsGroup, groupBoundaries, object->startsGroup = true, randomAccessActive in CAPTURE and HDR, the `if (catalog.randomAccess)` gate.)

- [ ] **Step 2: Run the guard**

Run: `chmod +x tests/keyframe-aligned-groups-guards.sh && bash tests/keyframe-aligned-groups-guards.sh`
Expected: `keyframe-aligned-groups guards passed`, exit 0. If a grep fails, the pattern doesn't match the committed source — read the source and correct ONLY the test pattern (do not change source).

- [ ] **Step 3: Run the full guard suite**

Run: `for t in tests/*.sh; do echo "== $t =="; bash "$t"; echo "rc=$?"; done`
Expected: every script exits 0 EXCEPT the two known pre-existing failures `build-debian-bookworm-script.sh` and `preferences-guards.sh` (unrelated to this work).

- [ ] **Step 4: Commit**

```bash
git add tests/keyframe-aligned-groups-guards.sh
git commit -m "Update keyframe-grouping guards for RAP-scan boundary detection"
```

---

## Self-Review notes

- Spec coverage: remove encode-time recording -> Task 1; video PID capture + scanForRapBoundaries + call site -> Task 2; rescan/no-double-count invariant -> Task 3; guard update -> Task 4. `readObject` is intentionally untouched (the design keeps it).
- Type consistency: new members `scannedTo` (uint64), `videoPidValue` (int); helper `scanForRapBoundaries()` (void, Impl member); `muxedProduced` fully removed; `groupBoundaries`/`muxedConsumed`/`sawVideoKeyframe`/`pcrPidValue` reused unchanged.
- First-RAP semantics preserved: scanner skips the first RAP (group 0 contains it), matching the prior `beforeSize > 0 || !groupBoundaries.empty()` guard intent, so `readObject` and group numbering need no change.
- Packet alignment: `muxedBytes` only ever shrinks by 188-multiples in `readObject`, so `muxedConsumed` is packet-aligned; the scanner re-aligns defensively and skips any non-0x47 window.
- Fallback: if no video PID and no pcrPid, scanner returns early; audio-only keeps a single group 0 and `randomAccessActive()` stays false.
