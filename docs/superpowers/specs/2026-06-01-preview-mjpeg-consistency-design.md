# Preview/publish capture-format consistency design

Status: design approved (sections), pending written-spec review
Date: 2026-06-01
Repo: moq2ts (this repo); Linux/V4L2 capture path only

## Problem

The camera MJPEG/framerate feature (spec 2026-05-31-camera-mjpeg-framerate)
taught the PUBLISH path to probe V4L2 modes, prefer MJPG, and select the
capable device node so 1080p captures at 30fps instead of 5fps. The live
PREVIEW path was not updated.

`LibavPreviewWorker::openStream` (src/media/LibavPreviewWorker.cpp) opens
`config.cameraDeviceId` directly, setting only `framerate` and `video_size`
with no `input_format` and no node selection. So when the selected camera's
persisted id is a raw-only / lower node, the preview can open a raw mode (e.g.
1080p@5) while publish opens the MJPG sibling at 30fps. The live preview then
does not represent the published stream: it looks choppy or runs at a different
rate than what is broadcast.

The selection logic already exists and is factored into
`src/media/V4l2Capabilities` (`groupNodesForCamera`, `queryModes`,
`selectBestMode`), but the *choosing* sequence is currently inlined only in
`LibavCaptureSource::Impl::addVideo`. The preview path has no access to it.

## Decision

Extract the choose-the-(node,format) sequence into one shared helper in
`V4l2Capabilities`, and route BOTH `addVideo` (capture) and
`LibavPreviewWorker` (preview) through it. Single source of truth so the two
paths cannot diverge again.

Chosen contract (from brainstorming):
- A shared `resolveCaptureOpen(cameraDeviceId, w, h, fps) -> {node, useMjpeg}`
  helper (Linux-only). It does the probing + selection but does NOT open the
  device; opening stays with each caller, which owns its own AVFormatContext
  and cleanup.
- Each caller keeps its own open + one-time MJPEG->raw retry.
- Preview does NOT re-encode (it only decodes to QImage), so the encoder
  rate-reconciliation half of the capture fix does not apply to preview.
- The capture diag line drops the selection-time `negotiated`/`meets` fields
  (the post-open reconciliation already logs the real negotiated fps).
- Linux-only; macOS/Windows preview and capture keep current behavior.

## Design

### C1. `resolveCaptureOpen` helper (src/media/V4l2Capabilities.h/.cpp)

Header, alongside the existing Linux-guarded declarations:

```cpp
struct CaptureOpen {
    std::string node;       // device node to open
    bool useMjpeg = false;  // true -> set input_format=mjpeg
};

#if defined(__linux__)
CaptureOpen resolveCaptureOpen(const std::string& cameraDeviceId,
                               int reqWidth, int reqHeight, double reqFps);
#endif
```

`.cpp`, inside the existing `#if defined(__linux__)` region — the block lifted
verbatim from addVideo's current inlined selection:

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

One responsibility: map a selected camera id + requested geometry to the
`(node, format)` to open. No libav, no Qt. Never fails: returns
`{cameraDeviceId, false}` when probing yields nothing.

### C2. Refactor addVideo (capture) to use the helper

In `LibavCaptureSource::Impl::addVideo`, replace the inlined Linux selection
block with a call to `resolveCaptureOpen`:

```cpp
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

This is a pure extraction: the `openWith` lambda, the MJPEG->raw retry, the
pre-retry input-state reset, and the rate reconciliation are all unchanged. The
diag line drops the `negotiated=%.1f meets=%d` fields (decision: post-open
reconciliation already logs the real negotiated fps). Requires
`#include "V4l2Capabilities.h"` (already present in LibavCaptureSource.cpp).

### C3. Preview worker selection (src/media/LibavPreviewWorker.cpp)

Do the selection in the CALLER (the `if (!config.cameraDeviceId.isEmpty())`
block at lines 354-362 in the run loop), so `openStream` stays generic. Add a
`bool preferMjpeg = false` parameter to `openStream`; when true and the media
type is video, set `av_dict_set(&options, "input_format", "mjpeg", 0)` before
`avformat_open_input`.

Rewrite the video-open block:

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
            // One-time raw fallback when MJPEG open failed.
            if (preferMjpeg) {
                videoStream = std::make_unique<PreviewStream>();  // ~PreviewStream frees the failed attempt
                if (!openStream(videoInputFormatName(), videoInputName(videoNode),
                                AVMEDIA_TYPE_VIDEO, config, videoStream.get(), false, &openError)) {
                    emit error(openError);
                    m_running.store(false, std::memory_order_release);
                    emit finished();
                    return;
                }
            } else {
                emit error(openError);
                m_running.store(false, std::memory_order_release);
                emit finished();
                return;
            }
        }
    }
```

`openStream` signature gains `bool preferMjpeg` (placed before `QString*
error`); update the audio call site to pass `false`. Inside `openStream`, the
video options block becomes:

```cpp
    if (mediaType == AVMEDIA_TYPE_VIDEO) {
        av_dict_set(&options, "framerate", QByteArray::number(config.videoFramerate).constData(), 0);
        av_dict_set(&options, "video_size", QStringLiteral("%1x%2").arg(config.videoWidth).arg(config.videoHeight).toUtf8().constData(), 0);
        if (preferMjpeg) {
            av_dict_set(&options, "input_format", "mjpeg", 0);
        }
    }
```

Requires `#include "V4l2Capabilities.h"` in LibavPreviewWorker.cpp.

Retry safety: `PreviewStream` has a destructor (lines 102-111) that frees
`format` via `avformat_close_input` and `decoder` via `avcodec_free_context`.
Replacing the `unique_ptr<PreviewStream>` for the retry destroys the failed
attempt cleanly, so no manual teardown is needed (unlike capture's StreamState,
which has no destructor and required an explicit reset).

Preview has no encoder, so there is no rate reconciliation; the generic decoder
already handles MJPG frames the same as raw (decode -> QImage), unchanged.

### C4. Platform guard

`resolveCaptureOpen` and `CaptureOpen` usage are `#if defined(__linux__)` /
`#if defined(Q_OS_LINUX)` guarded. `CaptureOpen` the struct may live outside the
guard in the header (it is a plain POD and harmless to declare everywhere);
only `resolveCaptureOpen` is Linux-guarded. Non-Linux: preview and capture open
`config.cameraDeviceId` raw with no input_format, exactly as today.

## Data flow

```
[preview start] LibavPreviewWorker(config), Linux:
  co = resolveCaptureOpen(cameraDeviceId, w, h, fps)        [C1 group+query+select]
  openStream(co.node, preferMjpeg=co.useMjpeg)
    -> avformat_open_input (input_format=mjpeg when useMjpeg)
    -> on fail & useMjpeg: fresh PreviewStream, openStream(co.node, preferMjpeg=false)
    -> find_stream_info -> best_stream -> decoder (mjpeg or raw, generic)
  decode loop: av_read_frame -> decode -> QImage -> videoFrameReady   [unchanged]

[publish start] addVideo, Linux:
  co = resolveCaptureOpen(cameraDeviceId, w, h, fps)        [C1 SAME helper]
  openWith(useMjpeg=co.useMjpeg) on co.node, raw-retry + reset        [unchanged]
  encoder rate reconciled from negotiated input rate                  [unchanged]
```

Both paths consult the identical `resolveCaptureOpen`, so for a given camera +
geometry they choose the same node/format: the preview displays what publish
will send.

## Error handling

- `resolveCaptureOpen` never fails (returns `{cameraDeviceId,false}` on empty
  probe). No error channel.
- Preview MJPEG-open failure: one-time raw fallback via a fresh PreviewStream;
  `~PreviewStream` frees the failed attempt, so no leak/double-open.
- Total preview failure (mjpeg and raw): existing `*error` ->
  `LibavPreviewWorker::error` signal -> panel status, unchanged.
- Capture path: unchanged; existing retry/reset/error semantics preserved by
  the extraction.

## Testing

- Unit: `selectBestMode` already has the standalone test
  (tests/v4l2_selection_test.cpp). `resolveCaptureOpen` is a thin Linux-only
  composition over ioctl/sysfs functions, not unit-testable without hardware;
  covered by the live probe + the existing selection test. No new unit test.
- Structural guard: extend tests/v4l2-capability-probe-guards.sh to assert:
  - `resolveCaptureOpen` is declared in src/media/V4l2Capabilities.h.
  - src/media/LibavPreviewWorker.cpp references `resolveCaptureOpen`.
  - src/media/LibavPreviewWorker.cpp references `input_format` (preview can no
    longer silently regress to raw-only).
- Compile: Docker bookworm build (both call sites; non-Linux guards intact).
- Live probe: reuse the existing probe to confirm
  `resolveCaptureOpen("/dev/videoN",1920,1080,30)` returns the MJPG node for an
  MJPG-capable camera.
- Manual acceptance: start preview on the camera; confirm ~30fps (smooth, not
  the old 5fps), matching the published stream's rate. Folds into the pending
  Task-6 manual run of the camera-fps feature.

Honest limitation: `resolveCaptureOpen` and the preview open path are not
unit-tested (device I/O); covered by structural guards + live probe + manual run
- the same coverage level as the rest of the capture/preview device code.

## Files

- Modify: src/media/V4l2Capabilities.h - add `struct CaptureOpen` and the
  Linux-guarded `resolveCaptureOpen` declaration.
- Modify: src/media/V4l2Capabilities.cpp - add `resolveCaptureOpen` definition
  (Linux region).
- Modify: src/media/LibavCaptureSource.cpp - replace addVideo's inlined
  selection block with a `resolveCaptureOpen` call; trim the diag line.
- Modify: src/media/LibavPreviewWorker.cpp - add `preferMjpeg` to `openStream`;
  select node/format via `resolveCaptureOpen` in the video-open block with a
  one-time raw fallback; include V4l2Capabilities.h.
- Modify: tests/v4l2-capability-probe-guards.sh - add the three assertions
  above.

## Out of scope (recorded)

- The pre-existing StreamState terminal-failure leak in LibavCaptureSource
  (noted in the camera-fps review; separate from this change).
- Egress burstiness / pacing.
- macOS/Windows capability probing (current behavior retained).
- Sharing the actual device-open (each caller keeps its own opener; only the
  selection is shared).

## Verification

- tests/v4l2-capability-probe-guards.sh passes with the new assertions; full
  guard suite passes; Docker bookworm build links.
- Live probe returns the MJPG node for an MJPG-capable camera.
- Manual: preview runs at ~30fps and matches the published rate.
