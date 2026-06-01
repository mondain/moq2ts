# Preview/Publish Capture-Format Consistency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the live preview select the same V4L2 node and pixel format (MJPG vs raw) as the publish path, so preview runs at the same framerate as the broadcast instead of opening a raw 5fps node.

**Architecture:** Extract the choose-the-(node,format) sequence currently inlined in `addVideo` into a shared `resolveCaptureOpen()` helper in `V4l2Capabilities`. Route both `LibavCaptureSource::Impl::addVideo` (publish) and `LibavPreviewWorker` (preview) through it; each caller keeps its own device open + one-time MJPEG->raw retry. Linux/V4L2 only; macOS/Windows unchanged.

**Tech Stack:** C++20, Linux V4L2, libav (v4l2 input), Qt 6, CMake + Docker bookworm build, bash structural guards.

**Spec:** `docs/superpowers/specs/2026-06-01-preview-mjpeg-consistency-design.md`

**Repo (absolute):** `/media/mondain/terrorbyte/workspace/github-moq/moq2ts`

---

## File Structure

- Modify `src/media/V4l2Capabilities.h` — add `struct CaptureOpen` (POD, unguarded) and the Linux-guarded `resolveCaptureOpen` declaration.
- Modify `src/media/V4l2Capabilities.cpp` — add `resolveCaptureOpen` definition in the existing `#if defined(__linux__)` region.
- Modify `src/media/LibavCaptureSource.cpp` — replace addVideo's inlined selection block with a `resolveCaptureOpen` call (trim diag).
- Modify `src/media/LibavPreviewWorker.cpp` — add `preferMjpeg` to `openStream`; select node/format via `resolveCaptureOpen` in the video-open block with a one-time raw fallback; include `V4l2Capabilities.h`.
- Modify `tests/v4l2-capability-probe-guards.sh` — add three assertions.

Both `src/media/*.cpp` already include sibling headers as `"V4l2Capabilities.h"`.

---

## Task 1: Add the shared resolveCaptureOpen helper and route addVideo through it

This task adds the helper and refactors the publish path to use it. Pure extraction — publish behavior is unchanged except the diag line drops two fields.

**Files:**
- Modify: `src/media/V4l2Capabilities.h`
- Modify: `src/media/V4l2Capabilities.cpp`
- Modify: `src/media/LibavCaptureSource.cpp`

- [ ] **Step 1: Declare CaptureOpen + resolveCaptureOpen in the header**

In `src/media/V4l2Capabilities.h`, immediately AFTER the `selectBestMode` declaration (the line `int reqWidth, int reqHeight, double reqFps);` at line 45) and BEFORE the `#if defined(__linux__)` block at line 47, insert:

```cpp

// Result of choosing how to open a camera: which node, and whether to request
// the MJPEG input format. POD; usable on all platforms (only resolveCaptureOpen
// below is Linux-only).
struct CaptureOpen {
    std::string node;       // device node to open
    bool useMjpeg = false;  // true -> set input_format=mjpeg
};
```

Then INSIDE the existing `#if defined(__linux__)` block (after the `groupNodesForCamera` declaration at line 52, before the `#endif` at line 53), add:

```cpp
// Probe the physical camera that owns `cameraDeviceId` and choose the best
// (node, pixel format) for the requested geometry. Never fails: returns
// {cameraDeviceId, false} when probing yields nothing.
CaptureOpen resolveCaptureOpen(const std::string& cameraDeviceId,
                               int reqWidth, int reqHeight, double reqFps);
```

- [ ] **Step 2: Define resolveCaptureOpen in the .cpp**

In `src/media/V4l2Capabilities.cpp`, inside the existing `#if defined(__linux__)` region (where `queryModes` and `groupNodesForCamera` are defined), add this function (place it after `groupNodesForCamera`'s definition, still before that region's `#endif`):

```cpp
CaptureOpen resolveCaptureOpen(const std::string& cameraDeviceId,
                               int reqWidth, int reqHeight, double reqFps) {
    const std::vector<std::string> group = groupNodesForCamera(cameraDeviceId);
    std::vector<V4l2NodeModes> candidates;
    candidates.reserve(group.size());
    for (const std::string& n : group) {
        candidates.push_back(V4l2NodeModes{n, queryModes(n)});
    }
    const V4l2Selection sel = selectBestMode(candidates, reqWidth, reqHeight, reqFps);
    CaptureOpen out;
    out.node = sel.node.empty() ? cameraDeviceId : sel.node;
    out.useMjpeg = sel.useMjpeg;
    return out;
}
```

- [ ] **Step 3: Replace addVideo's inlined selection block with the helper**

In `src/media/LibavCaptureSource.cpp`, find this exact block in `addVideo` (currently lines 307-331):

```cpp
        // Choose the best (node, pixel format) for the requested geometry.
        QString chosenNode = config.cameraDeviceId;
        bool useMjpeg = false;
#if defined(Q_OS_LINUX)
        {
            const std::vector<std::string> group =
                groupNodesForCamera(config.cameraDeviceId.toStdString());
            std::vector<V4l2NodeModes> candidates;
            candidates.reserve(group.size());
            for (const std::string& n : group) {
                candidates.push_back(V4l2NodeModes{n, queryModes(n)});
            }
            const V4l2Selection sel = selectBestMode(
                candidates, config.videoWidth, config.videoHeight,
                static_cast<double>(config.videoFramerate));
            if (!sel.node.empty()) {
                chosenNode = QString::fromStdString(sel.node);
            }
            useMjpeg = sel.useMjpeg;
            std::fprintf(stderr,
                         "[moqxr][capture] selected node=%s mjpeg=%d targetFps=%d negotiated=%.1f meets=%d\n",
                         chosenNode.toUtf8().constData(), useMjpeg ? 1 : 0,
                         config.videoFramerate, sel.negotiatedFps, sel.meetsTarget ? 1 : 0);
        }
#endif
```

Replace it with:

```cpp
        // Choose the best (node, pixel format) for the requested geometry.
        QString chosenNode = config.cameraDeviceId;
        bool useMjpeg = false;
#if defined(Q_OS_LINUX)
        {
            const CaptureOpen co = resolveCaptureOpen(
                config.cameraDeviceId.toStdString(),
                config.videoWidth, config.videoHeight,
                static_cast<double>(config.videoFramerate));
            chosenNode = QString::fromStdString(co.node);
            useMjpeg = co.useMjpeg;
            std::fprintf(stderr, "[moqxr][capture] selected node=%s mjpeg=%d targetFps=%d\n",
                         chosenNode.toUtf8().constData(), useMjpeg ? 1 : 0, config.videoFramerate);
        }
#endif
```

(`#include "V4l2Capabilities.h"` is already at the top of this file; no include change needed. The `openWith` lambda, MJPEG->raw retry, input-state reset, and rate reconciliation that follow are unchanged.)

- [ ] **Step 4: Build via Docker bookworm**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...moq2ts-publisher-bookworm`, exit 0.

- [ ] **Step 5: Verify the selection unit test still passes (selectBestMode untouched, but confirm the .cpp still compiles standalone)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
g++ -std=c++20 -I src tests/v4l2_selection_test.cpp src/media/V4l2Capabilities.cpp -o /tmp/v4l2sel_pv1 && /tmp/v4l2sel_pv1; echo "test_exit=$?"
```
Expected: `test_exit=0`, no FAIL lines. (This also confirms the new resolveCaptureOpen/CaptureOpen additions compile in the standalone test build; resolveCaptureOpen is Linux-guarded so it is included on this Linux host.)

- [ ] **Step 6: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/V4l2Capabilities.h src/media/V4l2Capabilities.cpp src/media/LibavCaptureSource.cpp
git commit -m "Extract resolveCaptureOpen and use it in capture"
```
Commit message EXACTLY that, NO Claude tagline/Co-Authored-By.

---

## Task 2: Route the preview worker through resolveCaptureOpen

Make the preview select the same node/format and open with input_format=mjpeg (raw fallback).

**Files:**
- Modify: `src/media/LibavPreviewWorker.cpp`

- [ ] **Step 1: Include the capability header**

In `src/media/LibavPreviewWorker.cpp`, add after the existing `#include "LibavPreviewWorker.h"` (line 1):

```cpp
#include "V4l2Capabilities.h"
```

- [ ] **Step 2: Add a preferMjpeg parameter to openStream**

`openStream`'s current signature (around line 115) is:

```cpp
bool openStream(const char* formatName,
                const QString& inputName,
                AVMediaType mediaType,
                const PublishConfig& config,
                PreviewStream* stream,
                QString* error) {
```

Change it to add `bool preferMjpeg` before `QString* error`:

```cpp
bool openStream(const char* formatName,
                const QString& inputName,
                AVMediaType mediaType,
                const PublishConfig& config,
                PreviewStream* stream,
                bool preferMjpeg,
                QString* error) {
```

- [ ] **Step 3: Honor preferMjpeg in the video options block**

Inside `openStream`, the current video options block (around lines 130-133) is:

```cpp
    if (mediaType == AVMEDIA_TYPE_VIDEO) {
        av_dict_set(&options, "framerate", QByteArray::number(config.videoFramerate).constData(), 0);
        av_dict_set(&options, "video_size", QStringLiteral("%1x%2").arg(config.videoWidth).arg(config.videoHeight).toUtf8().constData(), 0);
    }
```

Replace with:

```cpp
    if (mediaType == AVMEDIA_TYPE_VIDEO) {
        av_dict_set(&options, "framerate", QByteArray::number(config.videoFramerate).constData(), 0);
        av_dict_set(&options, "video_size", QStringLiteral("%1x%2").arg(config.videoWidth).arg(config.videoHeight).toUtf8().constData(), 0);
        if (preferMjpeg) {
            av_dict_set(&options, "input_format", "mjpeg", 0);
        }
    }
```

- [ ] **Step 4: Select node/format and open with raw fallback in the video-open block**

In the run loop, the current video-open block (lines 354-362) is:

```cpp
    if (!config.cameraDeviceId.isEmpty()) {
        videoStream = std::make_unique<PreviewStream>();
        if (!openStream(videoInputFormatName(), videoInputName(config.cameraDeviceId), AVMEDIA_TYPE_VIDEO, config, videoStream.get(), &openError)) {
            emit error(openError);
            m_running.store(false, std::memory_order_release);
            emit finished();
            return;
        }
    }
```

Replace it with:

```cpp
    if (!config.cameraDeviceId.isEmpty()) {
        QString videoNode = config.cameraDeviceId;
        bool preferMjpeg = false;
#if defined(Q_OS_LINUX)
        const CaptureOpen co = resolveCaptureOpen(
            config.cameraDeviceId.toStdString(),
            config.videoWidth, config.videoHeight,
            static_cast<double>(config.videoFramerate));
        videoNode = QString::fromStdString(co.node);
        preferMjpeg = co.useMjpeg;
#endif
        videoStream = std::make_unique<PreviewStream>();
        if (!openStream(videoInputFormatName(), videoInputName(videoNode),
                        AVMEDIA_TYPE_VIDEO, config, videoStream.get(), preferMjpeg, &openError)) {
            // One-time raw fallback when MJPEG open failed; ~PreviewStream frees
            // the failed attempt when the unique_ptr is replaced.
            bool recovered = false;
            if (preferMjpeg) {
                videoStream = std::make_unique<PreviewStream>();
                recovered = openStream(videoInputFormatName(), videoInputName(videoNode),
                                       AVMEDIA_TYPE_VIDEO, config, videoStream.get(), false, &openError);
            }
            if (!recovered) {
                emit error(openError);
                m_running.store(false, std::memory_order_release);
                emit finished();
                return;
            }
        }
    }
```

- [ ] **Step 5: Update the audio openStream call site to pass preferMjpeg=false**

The audio-open block (lines 363-371) currently calls:

```cpp
        if (!openStream(audioInputFormatName(), audioInputName(config.microphoneDeviceId), AVMEDIA_TYPE_AUDIO, config, audioStream.get(), &openError)) {
```

Change that call to pass `false` for the new parameter:

```cpp
        if (!openStream(audioInputFormatName(), audioInputName(config.microphoneDeviceId), AVMEDIA_TYPE_AUDIO, config, audioStream.get(), false, &openError)) {
```

- [ ] **Step 6: Confirm no other openStream call sites remain**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
grep -n "openStream(" src/media/LibavPreviewWorker.cpp
```
Expected: exactly the function definition plus the two call sites (video + audio), all now with the `preferMjpeg` argument. If any call site still has the old 6-argument form, fix it to pass the bool.

- [ ] **Step 7: Build via Docker bookworm**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...`, exit 0.

- [ ] **Step 8: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/LibavPreviewWorker.cpp
git commit -m "Select MJPEG node in the preview path"
```
Commit message EXACTLY that, NO Claude tagline/Co-Authored-By.

---

## Task 3: Guard the shared helper and preview selection

**Files:**
- Modify: `tests/v4l2-capability-probe-guards.sh`

- [ ] **Step 1: Add three assertions to the guard script**

`tests/v4l2-capability-probe-guards.sh` currently ends with:

```bash
# The probe path must be Linux-guarded.
grep -q '#if defined(__linux__)' "$CAPS_CPP" \
  || fail "queryModes/groupNodesForCamera must be guarded for Linux"

printf 'v4l2-capability-probe guards passed\n'
```

Insert these assertions BEFORE the final `printf` line (after the Linux-guard check):

```bash
PREVIEW="$REPO_ROOT/src/media/LibavPreviewWorker.cpp"

grep -q 'CaptureOpen resolveCaptureOpen' "$CAPS_HDR" \
  || fail "V4l2Capabilities must declare resolveCaptureOpen"

grep -q 'resolveCaptureOpen' "$PREVIEW" \
  || fail "preview must select node/format via resolveCaptureOpen"

grep -q 'input_format' "$PREVIEW" \
  || fail "preview must request the MJPEG input_format when selected"

```

So the tail of the script reads: the Linux-guard check, then the three new checks, then `printf 'v4l2-capability-probe guards passed\n'`.

- [ ] **Step 2: Run the guard and the full suite**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash tests/v4l2-capability-probe-guards.sh; echo "rc=$?"
for t in tests/*.sh; do
  out=$(bash "$t" 2>&1); rc=$?
  printf "%-50s %s\n" "$(basename "$t")" "$([ $rc -eq 0 ] && echo PASS || echo "FAIL(rc=$rc)")"
done
```
Expected: the new guard prints `v4l2-capability-probe guards passed` (rc=0); every script in the suite PASS.

- [ ] **Step 3: Live probe — confirm resolveCaptureOpen picks the MJPG node**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
cat > /tmp/probe_resolve.cpp <<'EOF'
#include "media/V4l2Capabilities.h"
#include <cstdio>
int main(int argc, char** argv) {
    const std::string id = argc > 1 ? argv[1] : "/dev/video2";
    moq2ts::CaptureOpen co = moq2ts::resolveCaptureOpen(id, 1920, 1080, 30.0);
    std::printf("id=%s -> node=%s mjpeg=%d\n", id.c_str(), co.node.c_str(), co.useMjpeg);
    return 0;
}
EOF
g++ -std=c++20 -I src /tmp/probe_resolve.cpp src/media/V4l2Capabilities.cpp -o /tmp/probe_resolve
/tmp/probe_resolve /dev/video0
/tmp/probe_resolve /dev/video2
```
Expected: for an MJPG-1080p30-capable camera, `node=` is that camera's MJPG node and `mjpeg=1`. Report the actual lines. (On the current host both attached cameras are MJPG-1080p-capable, so both should show `mjpeg=1`.) Do NOT commit `/tmp/probe_resolve.cpp`.

- [ ] **Step 4: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add tests/v4l2-capability-probe-guards.sh
git commit -m "Guard the shared capture-open selection for preview"
```
Commit message EXACTLY that, NO Claude tagline/Co-Authored-By.

---

## Task 4: Manual acceptance verification

**Files:** none (manual run; user-performed).

- [ ] **Step 1: Confirm the build is current**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...`, exit 0.

- [ ] **Step 2: Run the preview (ask the user to perform this)**

Launch the bookworm publisher, select the camera, and start the PREVIEW (do not need to publish). Observe the preview pane.

Success criteria:
- The preview is smooth (~30fps), not the old ~5fps choppiness.
- If you then also publish, the preview rate matches the broadcast rate.
- stderr shows the preview opened the MJPG node (no `v4l2 ... time per frame changed from 1/30 to 1/5` line for the preview open).

- [ ] **Step 3: Report results**

Summarize observed preview smoothness and whether preview and publish now use the same node/format. If the preview still opens raw/5fps, report the stderr lines so the selection path can be investigated.

---

## Notes for the implementer

- Single repo (moq2ts); commit all tasks here. moqxr untouched.
- Do NOT add Claude author taglines / "Generated with" / "Co-Authored-By" to commits.
- `resolveCaptureOpen` is Linux-guarded (`#if defined(__linux__)`); call sites use `#if defined(Q_OS_LINUX)` (which implies `__linux__`). The `CaptureOpen` struct is intentionally NOT guarded so it can be named in unguarded code if needed.
- Preview's `PreviewStream` has a destructor (frees `format`/`decoder`), so the MJPEG->raw retry is clean by replacing the `unique_ptr` — no manual teardown needed (this differs from capture's `StreamState`, which has none).
- `/tmp/probe_resolve.cpp` is throwaway; do not commit it.
- Do not change addVideo's retry/reset/rate-reconciliation logic — Task 1 is a pure extraction of the selection block only.
