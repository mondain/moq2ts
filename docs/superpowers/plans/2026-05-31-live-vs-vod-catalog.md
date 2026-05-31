# Live vs VOD catalog handling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Emit a spec-correct catalog that distinguishes live (capture) from VOD (file) publishes per MSF `isLive`-conditional fields and MSFTS Section 7.3.

**Architecture:** `LibavCaptureSource` -> `isLive=true`. `M2tsPacketizer` (file) -> `isLive=false`, `randomAccess=true`, and `trackDurationMs` from a new isolated libav probe. `MsftsMuxer::catalogJson` gains emission of `trackDuration` (VOD only) and `m2tsRandomAccess`, reusing the existing `isLive` gating it already applies to `generatedAt`/`targetLatency`.

**Tech Stack:** C++20, Qt 6, FFmpeg/libav (already linked target-wide; guarded by `MOQ2TS_HAVE_LIBAV_CAPTURE`, applied to the whole `moq2ts-publisher` target in CMakeLists.txt:154 — no CMake change needed). Tests are bash source-guard scripts under `tests/` + the Docker build (`scripts/build-debian-bookworm.sh`).

**Reference:** `docs/superpowers/specs/2026-05-31-live-vs-vod-catalog-design.md`; drafts `docs/draft-ietf-moq-msf-00.txt` (5.1.6, 5.1.15, 5.1.16, 5.1.37) and `docs/draft-gregoire-moq-msfts-00.txt` (6.8, 7.3).

---

## File Structure

- `src/media/MsftsMuxer.h` — `MsftsCatalog` gains `qint64 trackDurationMs = 0;` and `bool randomAccess = false;`.
- `src/media/MsftsMuxer.cpp` — `catalogJson` emits `m2tsRandomAccess` and (VOD-only) `trackDuration`.
- `src/media/M2tsPacketizer.h` — declare `static qint64 probeDurationMs(const QString& sourcePath);`.
- `src/media/M2tsPacketizer.cpp` — implement the libav duration probe behind `MOQ2TS_HAVE_LIBAV_CAPTURE`.
- `src/media/LivePipeline.cpp` — capture branch `isLive=true`; file branch `isLive=false`, `randomAccess=true`, `trackDurationMs` from probe.
- `tests/live-vod-catalog-guards.sh` — source guards.

---

## Task 1: Add trackDurationMs and randomAccess to MsftsCatalog

**Files:**
- Modify: `src/media/MsftsMuxer.h:26-28` (after `generatedAtMs`, `format`, `namespaceName`)

- [ ] **Step 1: Add the fields**

In `src/media/MsftsMuxer.h`, inside `struct MsftsCatalog`, after `QString namespaceName;`, add:

```cpp
    // VOD-only track duration in integer milliseconds (MSF 5.1.37); emitted only
    // when isLive is false and the value is > 0.
    qint64 trackDurationMs = 0;
    // When true, advertise m2tsRandomAccess (MSFTS 6.8): every MOQT group begins
    // at a random-access point.
    bool randomAccess = false;
```

- [ ] **Step 2: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete`, `SCRIPT_EXIT=0`, `error_count=0` (defaulted fields are source-compatible with existing designated initializers).

- [ ] **Step 3: Commit**

```bash
git add src/media/MsftsMuxer.h
git commit -m "Add trackDurationMs and randomAccess to MsftsCatalog"
```

---

## Task 2: Emit m2tsRandomAccess and trackDuration in catalogJson

**Files:**
- Modify: `src/media/MsftsMuxer.cpp` — m2ts field block (after the `m2tsTimestampMode` conditional, before `initData`, ~line 38) and the root block (~line 63-66)

- [ ] **Step 1: Emit m2tsRandomAccess**

In `src/media/MsftsMuxer.cpp`, immediately after the `m2tsTimestampMode` conditional block (the `if (catalog.packetSize == 192 ...) { ... }`) and before the `initData` block, add:

```cpp
    // MSFTS 6.8: only advertised when every group begins at a random-access point.
    if (catalog.randomAccess) {
        mediaTrack.insert(QStringLiteral("m2tsRandomAccess"), true);
    }
    // MSF 5.1.37: track duration is VOD-only (MUST NOT appear when isLive true).
    if (!catalog.isLive && catalog.trackDurationMs > 0) {
        mediaTrack.insert(QStringLiteral("trackDuration"), catalog.trackDurationMs);
    }
```

- [ ] **Step 2: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete`, `error_count=0`.

- [ ] **Step 3: Render both catalog variants with a host harness**

```bash
cat > /tmp/lv.cpp <<'EOF'
#include <QByteArray>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <cstdio>
#include "/media/mondain/terrorbyte/workspace/github-moq/moq2ts/src/media/MsftsMuxer.cpp"
static void dump(const char* label, const moq2ts::MsftsCatalog& c) {
    QByteArray j = moq2ts::MsftsMuxer::catalogJson(c);
    printf("== %s ==\n%s\n", label,
           QJsonDocument(QJsonDocument::fromJson(j).object()).toJson(QJsonDocument::Indented).constData());
}
int main() {
    using namespace moq2ts;
    MsftsCatalog live;
    live.track = "program-1"; live.timelineTrack = "program-1.timeline";
    live.packetSize = 188; live.namespaceName = "live/paul1";
    live.bitrateBps = 2500000; live.generatedAtMs = 1748600000000LL;
    live.isLive = true;
    dump("LIVE", live);

    MsftsCatalog vod = live;
    vod.isLive = false; vod.randomAccess = true; vod.trackDurationMs = 632000;
    vod.generatedAtMs = 1748600000000LL;  // present but must be suppressed by isLive=false
    dump("VOD", vod);
    return 0;
}
EOF
g++ -std=c++20 -fPIC /tmp/lv.cpp -o /tmp/lv $(pkg-config --cflags --libs Qt6Core) && /tmp/lv
rm -f /tmp/lv /tmp/lv.cpp
```
Expected:
- LIVE block contains `"isLive": true`, `"generatedAt"`, `"targetLatency"`; does NOT contain `"trackDuration"`, `"m2tsRandomAccess"`.
- VOD block contains `"isLive": false`, `"m2tsRandomAccess": true`, `"trackDuration": 632000`; does NOT contain `"generatedAt"` or `"targetLatency"`.

- [ ] **Step 4: Commit**

```bash
git add src/media/MsftsMuxer.cpp
git commit -m "Emit m2tsRandomAccess and VOD-only trackDuration in catalog"
```

---

## Task 3: Add libav duration probe to M2tsPacketizer

**Files:**
- Modify: `src/media/M2tsPacketizer.h:25-30` (public accessors)
- Modify: `src/media/M2tsPacketizer.cpp:1-8` (includes) and add the implementation

- [ ] **Step 1: Declare the probe**

In `src/media/M2tsPacketizer.h`, in the `public:` section after `std::uint64_t objectsRead() const;`, add:

```cpp
    // Probes the file with libav and returns its duration in integer
    // milliseconds, or 0 if unknown / libav unavailable / on any failure.
    static qint64 probeDurationMs(const QString& sourcePath);
```

- [ ] **Step 2: Add libav includes guarded by the build macro**

In `src/media/M2tsPacketizer.cpp`, after the existing includes (after `#include <utility>`), add:

```cpp
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif
```

- [ ] **Step 3: Implement the probe**

In `src/media/M2tsPacketizer.cpp`, add at file scope inside `namespace moq2ts` (e.g. just before the closing `} // namespace moq2ts`):

```cpp
qint64 M2tsPacketizer::probeDurationMs(const QString& sourcePath) {
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    if (sourcePath.isEmpty()) {
        return 0;
    }
    AVFormatContext* ctx = nullptr;
    const QByteArray path = sourcePath.toUtf8();
    if (avformat_open_input(&ctx, path.constData(), nullptr, nullptr) != 0) {
        return 0;
    }
    qint64 durationMs = 0;
    if (avformat_find_stream_info(ctx, nullptr) >= 0 && ctx->duration > 0) {
        // AVFormatContext::duration is in AV_TIME_BASE units (microseconds).
        durationMs = static_cast<qint64>((ctx->duration + (AV_TIME_BASE / 2000)) / (AV_TIME_BASE / 1000));
    }
    avformat_close_input(&ctx);
    return durationMs;
#else
    Q_UNUSED(sourcePath);
    return 0;
#endif
}
```

Note: `AV_TIME_BASE` is 1000000, so `AV_TIME_BASE/1000 == 1000` (us->ms divisor) and `AV_TIME_BASE/2000 == 500` (rounding). `QByteArray` keeps the UTF-8 buffer alive for the `avformat_open_input` call.

- [ ] **Step 4: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete`, `error_count=0`.

- [ ] **Step 5: Verify the probe against a real TS file (if one is available)**

```bash
# Substitute a known .ts asset path; expects a plausible non-zero ms value.
ls *.ts ../*.ts 2>/dev/null | head -1
```
If a file is available, the integration is exercised end-to-end in Task 4's run;
no standalone harness here because the probe needs libav + a real container.

- [ ] **Step 6: Commit**

```bash
git add src/media/M2tsPacketizer.h src/media/M2tsPacketizer.cpp
git commit -m "Add libav duration probe to M2tsPacketizer"
```

---

## Task 4: Wire isLive / randomAccess / trackDuration in LivePipeline

**Files:**
- Modify: `src/media/LivePipeline.cpp` — capture-branch catalog call (~lines 217-228) and file-branch catalog call (lines 223-236)

- [ ] **Step 1: Capture branch is explicitly live**

In `src/media/LivePipeline.cpp`, in the capture-path `MsftsMuxer::catalogJson({...})` call (the one using `capture.*`), add `.isLive = true,` immediately after `.namespaceName = m_config.namespaceName,`:

```cpp
            .namespaceName = m_config.namespaceName,
            .isLive = true,
            .bitrateBps = static_cast<qint64>(m_config.videoTargetBitrateKbps) * 1000,
            .generatedAtMs = QDateTime::currentMSecsSinceEpoch(),
```

(Designated initializers must stay in declaration order: `isLive` is declared
before `bitrateBps`/`generatedAtMs` in the struct, so it goes here.)

- [ ] **Step 2: File branch is VOD with duration + random access**

In the file-path `MsftsMuxer::catalogJson({...})` call (lines 223-236, using `packetizer.*`), change the tail. Replace:

```cpp
        .timelineTrack = timelineTrackName,
        .namespaceName = m_config.namespaceName,
        .bitrateBps = static_cast<qint64>(m_config.videoTargetBitrateKbps) * 1000,
        .generatedAtMs = QDateTime::currentMSecsSinceEpoch(),
    });
```

with:

```cpp
        .timelineTrack = timelineTrackName,
        .namespaceName = m_config.namespaceName,
        .isLive = false,
        .bitrateBps = static_cast<qint64>(m_config.videoTargetBitrateKbps) * 1000,
        // generatedAt is suppressed for VOD by catalogJson (isLive false).
        .trackDurationMs = M2tsPacketizer::probeDurationMs(sourcePath),
        .randomAccess = true,
    });
```

Note: `generatedAtMs` is intentionally dropped from the VOD initializer (it would
be suppressed anyway, but omitting it is clearer). `sourcePath` is the local
already computed at the top of the file branch (`const QString sourcePath = ...`).

- [ ] **Step 3: Verify build**

Run: `bash scripts/build-debian-bookworm.sh`
Expected: `Build complete`, `error_count=0`.

- [ ] **Step 4: Functional check**

Publish a file source and confirm (relay log or local catalog dump) the catalog
shows `isLive:false`, `m2tsRandomAccess:true`, a non-zero `trackDuration` (if the
file has a readable duration), and no `generatedAt`/`targetLatency`. Publish a
capture source and confirm `isLive:true` with `generatedAt`/`targetLatency` and no
`trackDuration`/`m2tsRandomAccess`.

- [ ] **Step 5: Commit**

```bash
git add src/media/LivePipeline.cpp
git commit -m "Mark capture publishes live and file publishes VOD in catalog"
```

---

## Task 5: Source guard test

**Files:**
- Create: `tests/live-vod-catalog-guards.sh`

- [ ] **Step 1: Write the guard**

Create `tests/live-vod-catalog-guards.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MUXER="$REPO_ROOT/src/media/MsftsMuxer.cpp"
HDR="$REPO_ROOT/src/media/MsftsMuxer.h"
PKT="$REPO_ROOT/src/media/M2tsPacketizer.cpp"
PIPE="$REPO_ROOT/src/media/LivePipeline.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

grep -q 'qint64 trackDurationMs' "$HDR" \
  || fail "MsftsCatalog must carry trackDurationMs"
grep -q 'bool randomAccess' "$HDR" \
  || fail "MsftsCatalog must carry randomAccess"

# trackDuration must be VOD-gated (never emitted when isLive is true).
grep -q '!catalog.isLive && catalog.trackDurationMs > 0' "$MUXER" \
  || fail "trackDuration must be gated on !isLive && trackDurationMs > 0"
grep -q 'if (catalog.randomAccess)' "$MUXER" \
  || fail "m2tsRandomAccess must be gated on catalog.randomAccess"

# Duration probe exists and is libav-guarded.
grep -q 'probeDurationMs' "$PKT" \
  || fail "M2tsPacketizer must implement probeDurationMs"
grep -q 'MOQ2TS_HAVE_LIBAV_CAPTURE' "$PKT" \
  || fail "probeDurationMs must be guarded by MOQ2TS_HAVE_LIBAV_CAPTURE"

# Pipeline marks file path VOD and capture path live.
grep -q '.randomAccess = true,' "$PIPE" \
  || fail "file path must advertise randomAccess for VOD"
grep -q '.trackDurationMs = M2tsPacketizer::probeDurationMs' "$PIPE" \
  || fail "file path must source trackDurationMs from the probe"
grep -q '.isLive = false,' "$PIPE" \
  || fail "file path must set isLive=false"
grep -q '.isLive = true,' "$PIPE" \
  || fail "capture path must set isLive=true"

printf 'live-vod-catalog guards passed\n'
```

- [ ] **Step 2: Run it (passes after Tasks 1-4)**

Run: `chmod +x tests/live-vod-catalog-guards.sh && bash tests/live-vod-catalog-guards.sh`
Expected: `live-vod-catalog guards passed`, exit 0.

- [ ] **Step 3: Run the full guard suite**

Run: `for t in tests/*.sh; do echo "== $t =="; bash "$t" || exit 1; done`
Expected: every script exits 0.

- [ ] **Step 4: Commit**

```bash
git add tests/live-vod-catalog-guards.sh
git commit -m "Add source guards for live vs VOD catalog"
```

---

## Self-Review notes

- Spec coverage: source-mode detection -> Task 4; isLive (5.1.15) -> Task 4 (both
  branches) + existing emit at MsftsMuxer.cpp:14; generatedAt (5.1.6) +
  targetLatency (5.1.16) suppression -> already gated on `isLive` in committed
  catalogJson, kept; trackDuration (5.1.37) -> Tasks 1,2,3,4; m2tsRandomAccess
  (MSFTS 6.8 / 7.3) -> Tasks 1,2,4; libav-probe-omit-on-fail -> Task 3 returns 0
  -> Task 2 omits the field.
- Type consistency: `trackDurationMs` (qint64), `randomAccess` (bool),
  `probeDurationMs(const QString&) -> qint64`, JSON keys `trackDuration` /
  `m2tsRandomAccess` used identically across tasks.
- Designated-initializer ordering verified against the struct field order in
  MsftsMuxer.h (isLive before bitrateBps/generatedAtMs; trackDurationMs and
  randomAccess after generatedAtMs/format/namespaceName).
- Accepted caveat (VOD m2tsRandomAccess optimistic until file-path keyframe
  grouping exists) is recorded in the spec; not silently dropped.
