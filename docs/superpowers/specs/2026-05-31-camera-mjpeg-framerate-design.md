# Camera MJPEG / framerate selection design

Status: design approved (sections), pending written-spec review
Date: 2026-05-31
Repo: moq2ts (this repo); Linux/V4L2 capture path only

## Problem

A live camera publish requested 1920x1080@30 but captured at 5fps. The publisher
log showed `[video4linux2,v4l2] The driver changed the time per frame from 1/30
to 1/5`, and the encoder ran far below target with ~6s GOPs.

Root cause (verified against the hardware with `v4l2-ctl --list-formats-ext` and a
libav probe):

1. `addVideo` opens the camera with no `input_format`, so libav uses the device's
   default raw format (YUYV). On this camera, **YUYV at 1080p is capped at 5fps**;
   30fps at 1080p exists **only in MJPG**.
2. The MJPG-capable modes live on a *different* device node (`/dev/video2`) than
   the one the app uses. `enumerateVideoInputs` filters to
   `isPrimaryV4l2Device` (sysfs `index == 0`), which keeps only `/dev/video0`
   (YUYV-only) and hides `/dev/video2`. So the 30fps node is unreachable.
3. Even when the driver forces 5fps, the encoder stays pinned to
   `config.videoFramerate` (30): `time_base {1,30}`, `framerate {30,1}`, GOP math
   x30. This mismatch yields wrong timestamps and a ~6s GOP instead of ~1s.

Confirmed fixable: `v4l2-ctl` negotiated MJPG 1920x1080@30 on `/dev/video2`, and
`ffmpeg -f v4l2 -input_format mjpeg -video_size 1920x1080 -framerate 30 -i
/dev/video2` captured 30 frames at real-time `speed=1x`.

## Decision

Add a Linux V4L2 capability-probing layer that feeds the existing capture open
path. Decode is unchanged: `pumpOnce` already does
`send_packet -> receive_frame -> encodeFrame` for any codec, so MJPG flows through
once the input opens as MJPG.

Chosen behavior (from brainstorming):
- Prefer MJPG, fall back to raw, automatically (no new user control).
- Probe nodes and pick the best `(node, format)` per physical camera; show one
  combo entry per camera.
- Reconcile the encoder to the negotiated framerate and surface the actual fps.
- Probe via Linux v4l2 ioctls directly.
- Add a standalone unit test for the pure selection logic.

Scope boundary: Linux/V4L2 only. macOS (`avfoundation`) and Windows (`dshow`)
keep current behavior; all new logic is `#if defined(Q_OS_LINUX)`-guarded.

## Design

### C1. `V4l2Capabilities` (new, Linux-only)

New files `src/media/V4l2Capabilities.h/.cpp`, guarded by
`#if defined(Q_OS_LINUX)`. Two clearly separated layers so the decision logic is
testable without hardware.

Pure types and selection (no I/O, no Qt, no libav -- so the unit test links
nothing heavy):

```cpp
struct V4l2Mode {
    std::uint32_t pixelFormat;  // V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV, ...
    int width;
    int height;
    double fps;
};

struct V4l2NodeModes {
    std::string node;            // "/dev/video2"
    std::vector<V4l2Mode> modes;
};

struct V4l2Selection {
    std::string node;            // chosen device node
    bool useMjpeg;               // true -> open with input_format=mjpeg
    double negotiatedFps;        // best fps the chosen mode supports at the size
    bool meetsTarget;            // negotiatedFps >= requested fps at requested size
};

// Pure: given candidate nodes' modes and a request, choose the node+format.
// Preference: meets-target-with-MJPEG > meets-target-with-raw >
// best-effort highest fps at requested size (MJPEG tiebreak) > {first node,raw}.
V4l2Selection selectBestMode(const std::vector<V4l2NodeModes>& candidates,
                             int reqWidth, int reqHeight, double reqFps);
```

ioctl layer (real device, returns empty on any failure):

```cpp
std::vector<V4l2Mode> queryModes(const std::string& devNode);
```

`queryModes` opens `devNode` `O_RDONLY` and walks `VIDIOC_ENUM_FMT` ->
`VIDIOC_ENUM_FRAMESIZES` (discrete) -> `VIDIOC_ENUM_FRAMEINTERVALS` (discrete),
converting each interval `den/num` to fps. Closes the fd. Any ioctl/open failure
returns an empty vector (caller treats empty as "unknown -> fall back to raw").

`selectBestMode` matching rules:
- A mode "matches the size" when width==reqWidth and height==reqHeight.
- "meets target" when a size-matching mode has fps >= reqFps (small epsilon).
- Among meeting modes, prefer MJPEG over raw; if none meets, choose the
  size-matching mode with the highest fps (MJPEG tiebreak); if no node reports
  any size-matching mode (or candidates empty), return `{candidates[0].node (or
  ""), useMjpeg=false, negotiatedFps=reqFps, meetsTarget=false}` so the caller
  falls back to today's raw open.

### C2. Camera grouping in `enumerateVideoInputs`

Replace the `isPrimaryV4l2Device` (index==0) gate (`LibavCaptureSource.cpp:988`).
New behavior (Linux branch):
- Collect candidate `/dev/videoN` nodes from libav's device list.
- Group nodes by physical camera: resolve
  `/sys/class/video4linux/videoN/device` to its parent device; nodes sharing a
  parent are one camera. If the link is unreadable, the node becomes its own
  standalone entry (never dropped for lack of grouping).
- Keep only nodes with non-empty `queryModes` (drops metadata-only nodes such as
  the empty-format `/dev/video1`,`/dev/video3`).
- Emit one `CaptureDevice` per camera.

`CaptureDevice` (`LibavCaptureSource.h:18`) gains a node list:

```cpp
struct CaptureDevice {
    QString id;                 // stable node for settings persistence (lowest capture node)
    QString description;
    QStringList candidateNodes; // all capture nodes for this physical camera
};
```

`id` stays a real node so existing QSettings persistence keeps working; selection
among `candidateNodes` happens at open. Non-Linux builds set `candidateNodes` to
`{id}`.

### C3. Best-mode selection in addVideo

In `addVideo` (`LibavCaptureSource.cpp:321`), before `openInput`:
- Resolve the chosen camera's `candidateNodes` (from the selected
  `CaptureDevice`; the config carries the selected node id, and the source can
  re-group or be handed the candidate list -- see Data flow).
- Build `std::vector<V4l2NodeModes>` by calling `queryModes` per candidate.
- `V4l2Selection sel = selectBestMode(candidates, config.videoWidth,
  config.videoHeight, config.videoFramerate);`
- Open `sel.node`. If `sel.useMjpeg`, add `av_dict_set(&options, "input_format",
  "mjpeg", 0)` alongside the existing `framerate` and `video_size` options.
- If probing yields nothing (non-Linux, ioctl failure, empty candidates), open
  the configured node with no `input_format` -- exactly today's behavior.
- MJPG-open retry: if the MJPG open fails, retry once with the raw fallback
  `(sel.node, no input_format)` before surfacing an error.

### C4. Rate reconciliation in addVideo

Today the encoder is pinned to `config.videoFramerate`
(`LibavCaptureSource.cpp:349-350,357-359`). After `openInput`, derive the rate
from the opened input stream:

```cpp
AVRational neg = stream->inputStream->avg_frame_rate;
if (neg.num <= 0 || neg.den <= 0) neg = stream->inputStream->r_frame_rate;
int negFps = (neg.num > 0 && neg.den > 0)
             ? std::max(1, (int)std::lround(av_q2d(neg)))
             : std::max(1, config.videoFramerate);
stream->encoder->time_base = AVRational{1, negFps};
stream->encoder->framerate = AVRational{negFps, 1};
// gopSize uses negFps instead of config.videoFramerate
```

The negotiated fps is surfaced to the UI. When `negFps < config.videoFramerate`
that is not an error -- it is logged/shown ("requested 30, capturing at N") so the
operator knows the target was not met.

### C5. UI / config touch

Minimal. The camera combo shows one entry per camera (C2). The actual negotiated
fps surfaces via the existing status/log path (the source-selection logging added
earlier is a natural home: extend the publish-start summary or emit a status line
after open). No new user controls -- prefer-MJPG + auto-node-pick is automatic.

## Data flow (open -> first encoded frame)

```
[enumeration]
  enumerateVideoInputs()
    -> per /dev/videoN: queryModes(node)                       [C1]
    -> drop nodes with no capture modes (metadata nodes)
    -> group nodes by sysfs USB parent                          [C2]
    -> emit one CaptureDevice per camera, candidateNodes=[...]
  combo: "HD Webcam" (candidates: /dev/video0, /dev/video2)

[publish start -> addVideo(), requested 1920x1080@30]
  candidates = { (video0, queryModes), (video2, queryModes) }
  sel = selectBestMode(candidates, 1920,1080,30)                [C1/C3]
    video0: YUYV 1080p->5fps (raw, misses)
    video2: MJPG 1080p->30/60 (meets, MJPEG)  <- chosen
    -> { node=/dev/video2, useMjpeg=true, negotiatedFps=30, meetsTarget=true }
  openInput("/dev/video2", { framerate=30, video_size=1920x1080,
                             input_format=mjpeg })               [C3]
    -> avformat_open_input -> find_stream_info -> av_find_best_stream
    -> decoder = MJPEG -> avcodec_open2                          [already generic]
  reconcileRate(): negFps=30 -> encoder time_base {1,30}, gop=30xInterval [C4]
    -> surface "capturing at 30fps" to panel/log
  addOutputStream() -> avformat_write_header

[runtime, per frame]
  pumpOnce(): av_read_frame (MJPG) -> send_packet -> receive_frame (YUV)
    -> encodeFrame -> sendEncoderFrame (x264 -> TS)             [unchanged]
  -> 30fps end-to-end
```

Everything downstream of `receive_frame` (encode, keyframe/GOP grouping, muxing,
the stop-cancellation work) is untouched.

## Error handling

- Probe failure (C1): `queryModes` returns empty; never fatal. All-empty camera
  degrades to today's raw open of the configured node.
- Grouping failure (C2): unreadable sysfs link -> node becomes its own entry, not
  dropped. Worst case is the pre-existing one-entry-per-node behavior.
- No mode meets target (C3): pick best achievable at the requested size
  (highest fps, MJPEG tiebreak); else configured node + raw. Requested fps is
  never silently assumed -- C4 reconciles to negotiated.
- MJPG open fails but raw works (C3): retry once on `(node, raw)` before error.
- Rate edge cases (C4): `avg_frame_rate` 0/0 or absurd -> `r_frame_rate` ->
  `config.videoFramerate`; gop clamps to >=1.
- Shortfall visibility: negotiated < requested is logged/surfaced, not an error.
- Non-Linux: entire probe/selection/grouping path is
  `#if defined(Q_OS_LINUX)`; macOS/Windows compile and run as today.

## Testing

### Standalone unit test for selectBestMode (new C++ test executable)

`selectBestMode` is pure (no Qt/libav/I-O), so it gets genuine coverage.
moq2ts builds no test executables today, so add minimal CMake wiring:
- A new `add_executable(moq2ts-v4l2-selection-tests
  tests/v4l2_selection_test.cpp src/media/V4l2Capabilities.cpp)` plus
  `add_test(...)`, gated so it only builds on Linux. The selection logic and
  `V4l2Mode`/`V4l2NodeModes`/`V4l2Selection` types must compile without Qt or
  libav (plain headers) so the test links nothing heavy.
- Test style mirrors moqxr's `tests/moqt_session_test.cpp`: a `main()` with
  `bool expect(cond, msg)` and `ok &=`.

Table-driven cases:
1. This-camera matrix: video0 YUYV{1080p@5, 480p@30}, video2 MJPG{1080p@30/60} +
   YUYV{1080p@5} -> request 1080p@30 -> selects video2, useMjpeg=true,
   negotiatedFps>=30, meetsTarget=true.
2. MJPEG-not-needed: a node offers raw 1080p@30 -> meets target with raw, useMjpeg
   may be false (raw preferred only when it also meets; MJPEG preferred when both
   meet -- assert the documented preference: MJPEG wins the tie).
3. No node meets target: only 1080p@5 anywhere -> meetsTarget=false, picks the
   5fps mode, negotiatedFps==5 (best effort), useMjpeg reflects which node had it.
4. Empty candidates / empty modes -> safe fallback: node=="" or first node,
   useMjpeg=false, meetsTarget=false.
5. Size mismatch only (camera lacks the requested size) -> falls back (no
   size-matching mode) to best-effort/raw per the documented rule.

### Structural guards (bash) + Docker build

`tests/v4l2-capability-probe-guards.sh` asserting the invariants:
- `selectBestMode` present in `V4l2Capabilities.h`.
- MJPEG preference branch present (`V4L2_PIX_FMT_MJPEG`).
- `isPrimaryV4l2Device` index==0 gate removed from `enumerateVideoInputs`.
- `input_format` + `mjpeg` set in `addVideo`.
- encoder rate derives from `avg_frame_rate` (reconciliation present).
- probe path guarded by `#if defined(Q_OS_LINUX)`.
- Docker bookworm build compiles and links (`scripts/build-debian-bookworm.sh`).

### Live probe (this hardware)

A one-shot diagnostic (throwaway, or a hidden `--probe-cameras` flag) that runs
`enumerateVideoInputs` + `queryModes` + `selectBestMode` and prints the chosen
node/format. Expected on this hardware: groups video0+video2 as one camera,
selects `/dev/video2` MJPEG for 1080p30. Cross-checked against the known
`v4l2-ctl` ground truth.

### Manual acceptance

Publish at 1920x1080: panel/log reports ~30fps (not 5); the `v4l2 ... time per
frame` downgrade line is gone from stderr; GOP diag shows ~1s spacing; libx264
`frame I:/P:` counts are consistent with ~30fps over the run.

### Honest limitations

The ioctl layer (`queryModes`) and sysfs node-grouping cannot be unit-tested
without real hardware; they are covered by the live probe + manual run + structural
guards -- the same coverage level as the rest of this app's capture code. The
selection logic, the part most worth testing, has genuine unit coverage.

## Files

- Create: `src/media/V4l2Capabilities.h` / `.cpp` -- V4l2Mode/NodeModes/Selection,
  `selectBestMode` (pure), `queryModes` (ioctls), Linux-guarded.
- Modify: `src/media/LibavCaptureSource.h` -- `CaptureDevice::candidateNodes`.
- Modify: `src/media/LibavCaptureSource.cpp` -- enumeration grouping (replace
  `isPrimaryV4l2Device`), best-mode selection + `input_format=mjpeg` in
  `addVideo`, rate reconciliation, surface actual fps.
- Modify: `CMakeLists.txt` -- compile `V4l2Capabilities.cpp` into the app; add the
  Linux-gated `moq2ts-v4l2-selection-tests` executable + `add_test`.
- Create: `tests/v4l2_selection_test.cpp` -- selectBestMode unit tests.
- Create: `tests/v4l2-capability-probe-guards.sh` -- structural guards.

## Out of scope (recorded)

- Egress burstiness / pacing (separate, already noted: per-GOP keyframe-flush
  clusters, paced=false).
- macOS/Windows capability probing (current behavior retained).
- User-facing pixel-format/mode picker (auto prefer-MJPG chosen instead).
- The relay-side playback path (separate investigation).

## Verification

- `moq2ts-v4l2-selection-tests` passes (table-driven selection cases).
- `tests/v4l2-capability-probe-guards.sh` passes; Docker bookworm build links.
- Live probe selects `/dev/video2` MJPEG for 1080p30 on this hardware.
- Manual: publish reports ~30fps, no driver-downgrade line, ~1s GOP.
