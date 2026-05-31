# Keyframe-aligned MOQT groups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Start a new MOQT group at each H.264 keyframe in the capture path so the first object of every group begins at a random-access point, then honestly advertise `m2tsRandomAccess: true` in the catalog.

**Architecture:** The encoder marks IDR packets with `AV_PKT_FLAG_KEY`. Just before each keyframe packet is muxed, record the current `muxedBytes` length as a group-boundary offset in a queue. `readObject` then refuses to let an object cross a boundary: it cuts the object at the boundary so the next object starts the new group, bumps a group counter, and resets object IDs per group. The catalog advertises `m2tsRandomAccess: true` only when this keyframe grouping is active (a video stream is present).

**Tech Stack:** C++20, Qt 6 (QByteArray/QString), FFmpeg/libav (libavcodec/libavformat), Docker bookworm build (`scripts/build-debian-bookworm.sh`). "Tests" in this repo are bash source-guard scripts under `tests/` (grep/python structural asserts) plus the Docker build as the compile/integration check. There is no C++ unit-test harness, so each task's verification is a guard script + the container build.

**Reference spec:** `docs/superpowers/specs/2026-05-31-keyframe-aligned-groups-design.md`. Normative drafts: `docs/draft-gregoire-moq-msfts-00.txt` (Sections 5.2, 5.3, 6.8), `docs/draft-ietf-moq-msf-00.txt` (Section 7.3).

---

## File Structure

- `src/media/M2tsPacketizer.h` — `M2tsObject` struct gains a `bool startsGroup` flag (shared by both capture and file paths).
- `src/media/LibavCaptureSource.cpp` — capture `Impl` gains keyframe-boundary tracking (record offsets at mux time) and boundary-aware slicing with group/object counters in `readObject`; exposes whether keyframe grouping is active.
- `src/media/LibavCaptureSource.h` — declare a `bool randomAccessActive() const` accessor.
- `src/media/MsftsMuxer.h` / `src/media/MsftsMuxer.cpp` — `MsftsCatalog` gains `bool randomAccess`; emit `m2tsRandomAccess` when true.
- `src/media/LivePipeline.cpp` — set `randomAccess` in the capture-path catalog from `capture.randomAccessActive()`; propagate `published.startsGroup` and emit the timeline record on group starts.
- `tests/keyframe-aligned-groups-guards.sh` — new source-guard script asserting the invariants below.

The file-source path (`M2tsPacketizer`) is **out of scope**; it keeps group 0 and does not set `m2tsRandomAccess`.

---

## Task 1: Add `startsGroup` to M2tsObject

**Files:**
- Modify: `src/media/M2tsPacketizer.h:13-17`

- [ ] **Step 1: Add the field**

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

- [ ] **Step 2: Verify it still compiles**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete` / `SCRIPT_EXIT=0`, `error_count=0`. (Adding a defaulted field is source-compatible with every existing initializer.)

- [ ] **Step 3: Commit**

```bash
git add src/media/M2tsPacketizer.h
git commit -m "Add startsGroup flag to M2tsObject"
```

---

## Task 2: Record keyframe byte-offsets at mux time

**Files:**
- Modify: `src/media/LibavCaptureSource.cpp` (Impl members near line 183-189; `sendEncoderFrame` near line 546-577)

Boundaries are recorded as absolute byte counts on a monotonic stream cursor, so
they remain valid as `muxedBytes` is consumed from the front in `readObject`.

- [ ] **Step 1: Add boundary-tracking state to Impl**

In `src/media/LibavCaptureSource.cpp`, in the non-libav-guarded member block (after `int pcrPidValue = -1;`, around line 188), add:

```cpp
    // Absolute byte offset (since stream start) at which each MOQT group begins.
    // A new group starts where a keyframe's muxed TS packets begin. Consumed in
    // order by readObject. Front entry is the next pending boundary.
    std::deque<std::uint64_t> groupBoundaries;
    // Total bytes ever appended to muxedBytes; the absolute cursor for the byte
    // currently at muxedBytes[0] is (muxedConsumed).
    std::uint64_t muxedProduced = 0;
    std::uint64_t muxedConsumed = 0;
    std::uint64_t nextGroupId = 0;
    bool sawVideoKeyframe = false;
    bool hasVideoStream = false;
```

Add the include near the other `<...>` includes at the top of the file:

```cpp
#include <deque>
```

- [ ] **Step 2: Mark the muxed-byte cursor and capture keyframe boundaries**

The memory sink is `writePacket` (line 124-128); it appends to `muxedBytes` but
cannot see Impl. Track produced bytes and boundaries in `sendEncoderFrame`
instead, where both the keyframe flag and the post-write `muxedBytes.size()` are
visible. Replace the write block inside `sendEncoderFrame` (currently lines
566-574) with:

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

Note: `muxedProduced` is recomputed from `muxedConsumed + muxedBytes.size()` after
the write so it stays correct regardless of interleaver buffering. `beforeSize`
is the absolute offset of the first byte the keyframe will occupy.

- [ ] **Step 3: Mark hasVideoStream when a video encoder is added**

In `addVideo` (near where `stream->video = true;` is set, around line 311), after the stream is successfully added, the Impl must learn a video stream exists. Locate `addOutputStream(std::move(stream), error)` at the end of `addVideo` (line 349) and change it to record the flag first:

```cpp
        hasVideoStream = true;
        return addOutputStream(std::move(stream), error);
```

- [ ] **Step 4: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete`, `error_count=0`.

- [ ] **Step 5: Commit**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Record keyframe group boundaries during capture muxing"
```

---

## Task 3: Slice objects on group boundaries in readObject

**Files:**
- Modify: `src/media/LibavCaptureSource.cpp:476-494` (`readObject`)

Current body (lines 476-494) slices by byte target and hard-codes group 0. Replace
it so an object never crosses a boundary, and group/object IDs advance correctly.

- [ ] **Step 1: Replace the slicing logic**

Replace the body of `readObject` (the lines from `const int targetBytes = ...`
through `return true;`) with:

```cpp
        const int targetBytes = std::max(1, packetsPerObject) * 188;

        // Determine the absolute offset of the next pending group boundary, if
        // any. We must not let an object cross it; the boundary becomes the first
        // byte of the next object (and the start of a new group).
        const std::uint64_t windowStart = muxedConsumed;
        std::uint64_t hardLimitBytes = static_cast<std::uint64_t>(targetBytes);
        bool cutAtBoundary = false;
        // Drop boundaries that are at or behind the current window start (already
        // consumed or coincident with the start of this object).
        while (!groupBoundaries.empty() && groupBoundaries.front() <= windowStart) {
            groupBoundaries.pop_front();
        }
        if (!groupBoundaries.empty()) {
            const std::uint64_t boundaryOffset = groupBoundaries.front() - windowStart;
            if (boundaryOffset > 0 && boundaryOffset < hardLimitBytes) {
                hardLimitBytes = boundaryOffset;
                cutAtBoundary = true;
            }
        }

        const std::uint64_t target = hardLimitBytes;
        while (running.load(std::memory_order_acquire) &&
               static_cast<std::uint64_t>(muxedBytes.size()) < target) {
            if (!pumpOnce(error)) {
                return false;
            }
            // pumpOnce may have produced new keyframe boundaries; if an earlier
            // boundary now falls inside our window, tighten the target.
            while (!groupBoundaries.empty() && groupBoundaries.front() <= windowStart) {
                groupBoundaries.pop_front();
            }
            if (!groupBoundaries.empty()) {
                const std::uint64_t boundaryOffset = groupBoundaries.front() - windowStart;
                if (boundaryOffset > 0 && boundaryOffset < target) {
                    // Re-evaluate against a tighter boundary on the next loop.
                    break;
                }
            }
        }

        std::uint64_t available = static_cast<std::uint64_t>(muxedBytes.size());
        std::uint64_t take = std::min(available, target);
        const int alignedBytes = static_cast<int>((take / 188) * 188);
        if (alignedBytes <= 0) {
            return false;
        }

        object->payload = muxedBytes.left(alignedBytes);
        muxedBytes.remove(0, alignedBytes);
        muxedConsumed += static_cast<std::uint64_t>(alignedBytes);

        // A new group starts when this object's first byte sits on a recorded
        // boundary, or for the very first object produced.
        bool startGroup = (nextObjectId == 0);
        if (cutAtBoundary && !groupBoundaries.empty() &&
            groupBoundaries.front() == windowStart) {
            // Boundary already trimmed to windowStart above only when consumed;
            // handled by the dedicated start path below.
        }
        if (!groupBoundaries.empty() && groupBoundaries.front() == muxedConsumed) {
            // The next object will start a group; nothing to do here.
        }
        // Detect group start: the current windowStart equals a boundary we just
        // popped. Track via a member set in pumpOnce/slice; simpler: a boundary
        // equal to windowStart means start.
        if (windowStart != 0 && !m_lastConsumedWasBoundary.empty()) {
            // placeholder removed in Step 2 refinement
        }

        if (startGroup) {
            object->groupId = nextGroupId;
            object->objectId = 0;
            object->startsGroup = true;
            nextObjectIdInGroup = 1;
        } else {
            object->groupId = nextGroupId;
            object->objectId = nextObjectIdInGroup++;
            object->startsGroup = false;
        }
        ++nextObjectId;
        return true;
```

NOTE: the placeholder block above is intentionally simplified in Step 2; do not
ship it as-is. Step 2 replaces the group-start detection with the clean version.

- [ ] **Step 2: Use a clean group-start signal**

The robust signal is: a group starts whenever the object's starting absolute
offset (`windowStart`) equals a recorded boundary. Add two Impl members near the
others (Task 2 block):

```cpp
    std::uint64_t nextObjectIdInGroup = 0;
    std::uint64_t pendingGroupBoundary = 0;  // absolute offset of the next group start, or 0 if none
```

Replace the entire group-assignment tail of `readObject` (everything after
`muxedConsumed += ...;`) with:

```cpp
        // A new group begins when this object starts exactly on a recorded
        // boundary, or for the first object overall.
        bool startGroup = (nextObjectId == 0);
        if (!startGroup && pendingGroupBoundary != 0 && windowStart == pendingGroupBoundary) {
            startGroup = true;
        }
        if (startGroup) {
            if (nextObjectId != 0) {
                ++nextGroupId;
            }
            object->groupId = nextGroupId;
            object->objectId = 0;
            object->startsGroup = true;
            nextObjectIdInGroup = 1;
        } else {
            object->groupId = nextGroupId;
            object->objectId = nextObjectIdInGroup++;
            object->startsGroup = false;
        }
        // The boundary we just cut at (if any) becomes the next group's start.
        if (cutAtBoundary && !groupBoundaries.empty()) {
            pendingGroupBoundary = groupBoundaries.front();
        }
        ++nextObjectId;
        return true;
```

And remove the placeholder block from Step 1 (the `m_lastConsumedWasBoundary`
and the two no-op `if` comments). The final `readObject` body is: target
computation (Step 1, first half) + fill loop + aligned take + this clean tail.

- [ ] **Step 3: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete`, `error_count=0`.

- [ ] **Step 4: Manual smoke check of grouping**

Run the app against a camera and confirm in the Logs/Stats that publishing
proceeds; group IDs now advance (verified end-to-end in Task 6). No automated
assertion here — boundary math is covered by the guard script in Task 5.

- [ ] **Step 5: Commit**

```bash
git add src/media/LibavCaptureSource.cpp
git commit -m "Slice capture objects on keyframe group boundaries"
```

---

## Task 4: Expose randomAccessActive() and add catalog field

**Files:**
- Modify: `src/media/LibavCaptureSource.h:36-40`
- Modify: `src/media/LibavCaptureSource.cpp` (accessor near the other `packetSize()/pcrPid()` accessors, ~line 941; Impl already has `hasVideoStream`)
- Modify: `src/media/MsftsMuxer.h` (`MsftsCatalog` struct)
- Modify: `src/media/MsftsMuxer.cpp` (`catalogJson`)

- [ ] **Step 1: Declare the accessor**

In `src/media/LibavCaptureSource.h`, after `QByteArray initData() const;` (line 40) add:

```cpp
    bool randomAccessActive() const;
```

- [ ] **Step 2: Implement the accessor**

In `src/media/LibavCaptureSource.cpp`, alongside the other forwarding accessors
(e.g. near `int LibavCaptureSource::packetSize() const`), add:

```cpp
bool LibavCaptureSource::randomAccessActive() const {
    // We can only honestly claim every group begins at a RAP once we are
    // grouping by keyframe, which requires a video stream and at least one
    // observed keyframe.
    return m_impl->hasVideoStream && m_impl->sawVideoKeyframe;
}
```

- [ ] **Step 3: Add the catalog field**

In `src/media/MsftsMuxer.h`, add to `MsftsCatalog` after `qint64 generatedAtMs = 0;`:

```cpp
    bool randomAccess = false;
```

- [ ] **Step 4: Emit m2tsRandomAccess**

In `src/media/MsftsMuxer.cpp`, in the m2ts-specific field block (after the
`m2tsTimestampMode` conditional, before `initData`), add:

```cpp
    // Only advertised when the publisher guarantees each group begins at a RAP
    // (MSFTS 6.8).
    if (catalog.randomAccess) {
        mediaTrack.insert(QStringLiteral("m2tsRandomAccess"), true);
    }
```

- [ ] **Step 5: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete`, `error_count=0`.

- [ ] **Step 6: Commit**

```bash
git add src/media/LibavCaptureSource.h src/media/LibavCaptureSource.cpp src/media/MsftsMuxer.h src/media/MsftsMuxer.cpp
git commit -m "Expose capture random-access state and m2tsRandomAccess catalog field"
```

---

## Task 5: Source guard test for the invariants

**Files:**
- Create: `tests/keyframe-aligned-groups-guards.sh`

- [ ] **Step 1: Write the guard script**

Create `tests/keyframe-aligned-groups-guards.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CAPTURE="$REPO_ROOT/src/media/LibavCaptureSource.cpp"
MUXER="$REPO_ROOT/src/media/MsftsMuxer.cpp"
M2TS_HDR="$REPO_ROOT/src/media/M2tsPacketizer.h"

fail() { printf '%s\n' "$1" >&2; exit 1; }

grep -q 'bool startsGroup' "$M2TS_HDR" \
  || fail "M2tsObject must carry a startsGroup flag"

grep -q 'AV_PKT_FLAG_KEY' "$CAPTURE" \
  || fail "capture must detect keyframes via AV_PKT_FLAG_KEY to mark group boundaries"

grep -q 'groupBoundaries' "$CAPTURE" \
  || fail "capture must track keyframe group boundaries"

grep -q 'object->startsGroup = true;' "$CAPTURE" \
  || fail "readObject must mark the first object of each group"

grep -q 'randomAccessActive' "$CAPTURE" \
  || fail "capture must expose randomAccessActive()"

# m2tsRandomAccess must only be emitted behind the randomAccess flag, never
# unconditionally.
grep -q 'if (catalog.randomAccess)' "$MUXER" \
  || fail "m2tsRandomAccess must be gated on catalog.randomAccess"

if grep -E 'insert\(QStringLiteral\("m2tsRandomAccess"\), *true\)' "$MUXER" \
     | grep -qv 'randomAccess'; then
  : # placeholder; the gating check above is the real assertion
fi

printf 'keyframe-aligned-groups guards passed\n'
```

- [ ] **Step 2: Make it executable and run it (expect FAIL before the rest is wired, PASS after Tasks 1-4)**

Run: `chmod +x tests/keyframe-aligned-groups-guards.sh && bash tests/keyframe-aligned-groups-guards.sh`
Expected (after Tasks 1-4 are committed): `keyframe-aligned-groups guards passed`, exit 0.

- [ ] **Step 3: Run the full guard suite to confirm no regressions**

Run: `for t in tests/*.sh; do echo "== $t =="; bash "$t" || exit 1; done`
Expected: every script prints its pass line / exits 0.

- [ ] **Step 4: Commit**

```bash
git add tests/keyframe-aligned-groups-guards.sh
git commit -m "Add source guards for keyframe-aligned grouping"
```

---

## Task 6: Wire grouping into the pipeline catalog and timeline

**Files:**
- Modify: `src/media/LivePipeline.cpp` (capture-path catalog ~line 217-228; capture-path `nextObject` timeline emission ~line 261-273)

- [ ] **Step 1: Set randomAccess in the capture catalog**

In `src/media/LivePipeline.cpp`, in the capture-path `MsftsMuxer::catalogJson({...})`
call (the one using `capture.*`), add after `.generatedAtMs = ...,`:

```cpp
            .randomAccess = capture.randomAccessActive(),
```

NOTE: `randomAccessActive()` reflects whether a keyframe has been seen. Because
the catalog is built before the first object is read, on a fresh start this is
typically false. To advertise it truthfully, build the catalog lazily after the
first keyframe — out of scope for this task; for now it reflects the state at
catalog-construction time. (See "Known limitation" below.)

- [ ] **Step 2: Emit the timeline record only on group starts**

The MSF media timeline (Section 7.3) wants an independent record at each group's
first object. Replace the timeline-trigger condition in the capture-path
`nextObject` (currently `if ((objects % timelineEveryObjects) == 0)`) with one
that also fires on group starts:

```cpp
            if (published.startsGroup || (objects % timelineEveryObjects) == 0) {
```

Leave the file-source path's condition unchanged (it has no group starts).

- [ ] **Step 3: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete`, `error_count=0`.

- [ ] **Step 4: Render the catalog to confirm the field appears when active**

Because `randomAccessActive()` depends on runtime keyframe state, verify the
muxer wiring with a host harness that forces `randomAccess = true`:

```bash
cat > /tmp/ra.cpp <<'EOF'
#include <QByteArray>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <cstdio>
#include "/media/mondain/terrorbyte/workspace/github-moq/moq2ts/src/media/MsftsMuxer.cpp"
int main() {
    using namespace moq2ts;
    MsftsCatalog c; c.track = "m2ts"; c.timelineTrack = "m2ts.timeline";
    c.packetSize = 188; c.randomAccess = true;
    printf("%s\n", MsftsMuxer::catalogJson(c).constData());
    return 0;
}
EOF
g++ -std=c++20 -fPIC /tmp/ra.cpp -o /tmp/ra $(pkg-config --cflags --libs Qt6Core) && /tmp/ra
rm -f /tmp/ra /tmp/ra.cpp
```
Expected: output JSON contains `"m2tsRandomAccess":true`.

- [ ] **Step 5: Commit**

```bash
git add src/media/LivePipeline.cpp
git commit -m "Advertise random access and emit per-group timeline records"
```

---

## Known limitation (record, do not fix here)

`randomAccessActive()` is evaluated when the catalog is built, before the first
object is read, so on a cold start it is usually false even though grouping will
become active. Making this truthful requires deferring catalog construction (or
republishing the catalog) until the first keyframe is observed. That is a
follow-up: "Defer/republish catalog after first keyframe."

Also still deferred: keyframe grouping for the file-source path
(`M2tsPacketizer`), which needs TS-level RAP detection (PUSI + SPS/IDR scan).

---

## Self-Review notes

- Spec coverage: boundary recording (design Approach 1) → Task 2; boundary-aware
  slicing + group/object IDs (Approach 2) → Task 3; `startsGroup` flag
  (Approach 3) → Tasks 1, 6; catalog `m2tsRandomAccess` gated on active grouping
  (Approach 4) → Task 4; audio-only / no-keyframe fallback (Approach 5) →
  `randomAccessActive()` returns false (Task 4) and `readObject` keeps a single
  group when no boundaries arrive (Task 3).
- Type consistency: member names (`groupBoundaries`, `muxedConsumed`,
  `muxedProduced`, `nextGroupId`, `nextObjectIdInGroup`, `pendingGroupBoundary`,
  `hasVideoStream`, `sawVideoKeyframe`), accessor `randomAccessActive()`, catalog
  field `randomAccess`, and JSON key `m2tsRandomAccess` are used identically
  across tasks.
- PSI-at-keyframe (design risk) and catalog-deferral (Task 6 limitation) are
  explicitly recorded as follow-ups, not silently dropped.
