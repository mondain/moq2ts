# Camera MJPEG / Framerate Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a 1080p camera publish actually capture at 30fps by probing V4L2 modes, preferring MJPG, selecting the capable device node per physical camera, and reconciling the encoder to the negotiated framerate.

**Architecture:** A new Linux-only `V4l2Capabilities` unit exposes a pure `selectBestMode()` (unit-tested) plus ioctl-based `queryModes()` and sysfs-based node grouping. `LibavCaptureSource` enumeration groups nodes per camera; `addVideo` probes candidates, opens the best node with `input_format=mjpeg` when MJPG wins, and derives the encoder rate from the negotiated input rate. Decode is unchanged — `pumpOnce` already decodes any codec.

**Tech Stack:** C++20, Linux V4L2 ioctls (`<linux/videodev2.h>`), libav (v4l2 input), Qt 6, CMake + CTest, Docker bookworm build + bash structural guards.

**Spec:** `docs/superpowers/specs/2026-05-31-camera-mjpeg-framerate-design.md`

**Repo (absolute):** `/media/mondain/terrorbyte/workspace/github-moq/moq2ts`

---

## File Structure

- Create `src/media/V4l2Capabilities.h` — pure types (`V4l2Mode`, `V4l2NodeModes`, `V4l2Selection`), pure `selectBestMode()`, and Linux declarations for `queryModes()` / `groupNodesForCamera()`. No Qt, no libav — so the unit test links nothing heavy.
- Create `src/media/V4l2Capabilities.cpp` — `selectBestMode()` (all platforms) + Linux-guarded `queryModes()` / `groupNodesForCamera()` ioctl/sysfs impl.
- Create `tests/v4l2_selection_test.cpp` — standalone unit test for `selectBestMode()`.
- Create `tests/v4l2-capability-probe-guards.sh` — structural guards.
- Modify `src/media/LibavCaptureSource.h` — `CaptureDevice::candidateNodes`.
- Modify `src/media/LibavCaptureSource.cpp` — enumeration grouping; `addVideo` selection + `input_format` + rate reconciliation.
- Modify `CMakeLists.txt` — add `V4l2Capabilities.cpp` to `SOURCES`, header to `HEADERS`, and a Linux-gated `moq2ts-v4l2-selection-tests` executable + `add_test`.

---

## Task 1: Pure selection logic `selectBestMode` + standalone unit test

This task is self-contained and platform-independent: it adds the header types and the pure function, a test executable, and proves the selection matrix. No libav/Qt/ioctl yet.

**Files:**
- Create: `src/media/V4l2Capabilities.h`
- Create: `src/media/V4l2Capabilities.cpp`
- Create: `tests/v4l2_selection_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create the header with pure types + declarations**

Create `src/media/V4l2Capabilities.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace moq2ts {

// One supported capture mode reported by a V4L2 node.
struct V4l2Mode {
    std::uint32_t pixelFormat = 0;  // V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV, ...
    int width = 0;
    int height = 0;
    double fps = 0.0;
};

struct V4l2NodeModes {
    std::string node;               // "/dev/video2"
    std::vector<V4l2Mode> modes;
};

struct V4l2Selection {
    std::string node;               // chosen device node ("" if none)
    bool useMjpeg = false;          // true -> open with input_format=mjpeg
    double negotiatedFps = 0.0;     // best fps achievable at the requested size
    bool meetsTarget = false;       // negotiatedFps >= requested fps at requested size
};

// V4L2_PIX_FMT_MJPEG fourcc ('M','J','P','G'); defined here so the pure logic
// and its test do not need <linux/videodev2.h>.
constexpr std::uint32_t kV4l2PixFmtMjpeg =
    (static_cast<std::uint32_t>('M')) |
    (static_cast<std::uint32_t>('J') << 8) |
    (static_cast<std::uint32_t>('P') << 16) |
    (static_cast<std::uint32_t>('G') << 24);

// Pure: choose the best (node, format) for the requested size/fps.
// Preference order:
//   1. meets target (fps >= req at the requested size) with MJPEG
//   2. meets target with raw
//   3. best-effort: highest fps at the requested size (MJPEG breaks ties)
//   4. fallback: first candidate node, raw, meetsTarget=false
// MJPEG wins when both an MJPEG and a raw mode meet the target.
V4l2Selection selectBestMode(const std::vector<V4l2NodeModes>& candidates,
                             int reqWidth, int reqHeight, double reqFps);

#if defined(__linux__)
// Query the discrete modes a node supports (ioctls). Empty on any failure.
std::vector<V4l2Mode> queryModes(const std::string& devNode);
// All capture nodes belonging to the same physical camera as `node`
// (shared sysfs USB parent). Returns {node} if grouping cannot be resolved.
std::vector<std::string> groupNodesForCamera(const std::string& node);
#endif

}  // namespace moq2ts
```

- [ ] **Step 2: Write the failing unit test**

Create `tests/v4l2_selection_test.cpp` (mirrors moqxr's plain-`main()` + `expect` style):

```cpp
#include "media/V4l2Capabilities.h"

#include <iostream>
#include <string>

using moq2ts::V4l2Mode;
using moq2ts::V4l2NodeModes;
using moq2ts::V4l2Selection;
using moq2ts::selectBestMode;
using moq2ts::kV4l2PixFmtMjpeg;

namespace {
constexpr std::uint32_t kYuyv =
    (std::uint32_t('Y')) | (std::uint32_t('U') << 8) |
    (std::uint32_t('Y') << 16) | (std::uint32_t('V') << 24);

bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}
}  // namespace

int main() {
    bool ok = true;

    // Case 1: this camera. video0 YUYV{1080p@5, 480p@30}; video2 MJPG{1080p@30,60}
    // + YUYV{1080p@5}. Request 1080p@30 -> video2 MJPEG, meets target.
    {
        std::vector<V4l2NodeModes> cands = {
            {"/dev/video0", {{kYuyv, 1920, 1080, 5.0}, {kYuyv, 640, 480, 30.0}}},
            {"/dev/video2", {{kV4l2PixFmtMjpeg, 1920, 1080, 30.0},
                             {kV4l2PixFmtMjpeg, 1920, 1080, 60.0},
                             {kYuyv, 1920, 1080, 5.0}}},
        };
        V4l2Selection s = selectBestMode(cands, 1920, 1080, 30.0);
        ok &= expect(s.node == "/dev/video2", "case1 picks video2");
        ok &= expect(s.useMjpeg, "case1 uses MJPEG");
        ok &= expect(s.meetsTarget, "case1 meets target");
        ok &= expect(s.negotiatedFps >= 30.0, "case1 negotiatedFps >= 30");
    }

    // Case 2: both MJPEG and raw meet target at the size -> MJPEG wins the tie.
    {
        std::vector<V4l2NodeModes> cands = {
            {"/dev/video0", {{kYuyv, 1280, 720, 30.0}}},
            {"/dev/video2", {{kV4l2PixFmtMjpeg, 1280, 720, 30.0}}},
        };
        V4l2Selection s = selectBestMode(cands, 1280, 720, 30.0);
        ok &= expect(s.useMjpeg, "case2 prefers MJPEG when both meet target");
        ok &= expect(s.meetsTarget, "case2 meets target");
    }

    // Case 2b: only raw meets target -> raw chosen, meets target, not MJPEG.
    {
        std::vector<V4l2NodeModes> cands = {
            {"/dev/video0", {{kYuyv, 1280, 720, 30.0}}},
        };
        V4l2Selection s = selectBestMode(cands, 1280, 720, 30.0);
        ok &= expect(s.node == "/dev/video0", "case2b picks the raw node");
        ok &= expect(!s.useMjpeg, "case2b uses raw");
        ok &= expect(s.meetsTarget, "case2b meets target with raw");
    }

    // Case 3: nothing meets target (only 1080p@5 anywhere) -> best effort 5fps.
    {
        std::vector<V4l2NodeModes> cands = {
            {"/dev/video0", {{kYuyv, 1920, 1080, 5.0}}},
        };
        V4l2Selection s = selectBestMode(cands, 1920, 1080, 30.0);
        ok &= expect(!s.meetsTarget, "case3 does not meet target");
        ok &= expect(s.node == "/dev/video0", "case3 picks the only node");
        ok &= expect(s.negotiatedFps == 5.0, "case3 negotiatedFps == 5 (best effort)");
    }

    // Case 4: empty candidates -> safe fallback.
    {
        V4l2Selection s = selectBestMode({}, 1920, 1080, 30.0);
        ok &= expect(s.node.empty(), "case4 empty node on no candidates");
        ok &= expect(!s.useMjpeg, "case4 raw fallback");
        ok &= expect(!s.meetsTarget, "case4 does not meet target");
    }

    // Case 5: requested size absent entirely -> fallback to first node, raw.
    {
        std::vector<V4l2NodeModes> cands = {
            {"/dev/video0", {{kYuyv, 640, 480, 30.0}}},
        };
        V4l2Selection s = selectBestMode(cands, 1920, 1080, 30.0);
        ok &= expect(s.node == "/dev/video0", "case5 falls back to first node");
        ok &= expect(!s.meetsTarget, "case5 cannot meet target at missing size");
    }

    return ok ? 0 : 1;
}
```

- [ ] **Step 3: Create the .cpp with the pure implementation only**

Create `src/media/V4l2Capabilities.cpp` (Linux ioctl/sysfs functions are added in Task 2; this step provides `selectBestMode` for all platforms):

```cpp
#include "media/V4l2Capabilities.h"

#include <algorithm>

namespace moq2ts {

namespace {
constexpr double kFpsEpsilon = 0.5;  // tolerate driver rounding (e.g. 29.97 ~ 30)

bool sizeMatches(const V4l2Mode& m, int w, int h) {
    return m.width == w && m.height == h;
}
}  // namespace

V4l2Selection selectBestMode(const std::vector<V4l2NodeModes>& candidates,
                             int reqWidth, int reqHeight, double reqFps) {
    V4l2Selection mjpegMeets;   bool haveMjpegMeets = false;
    V4l2Selection rawMeets;     bool haveRawMeets = false;
    V4l2Selection bestEffort;   bool haveBestEffort = false;

    for (const auto& cand : candidates) {
        for (const auto& m : cand.modes) {
            if (!sizeMatches(m, reqWidth, reqHeight)) {
                continue;
            }
            const bool isMjpeg = (m.pixelFormat == kV4l2PixFmtMjpeg);
            const bool meets = m.fps + kFpsEpsilon >= reqFps;

            if (meets && isMjpeg && !haveMjpegMeets) {
                mjpegMeets = {cand.node, true, m.fps, true};
                haveMjpegMeets = true;
            } else if (meets && !isMjpeg && !haveRawMeets) {
                rawMeets = {cand.node, false, m.fps, true};
                haveRawMeets = true;
            }
            // Track best-effort (highest fps at the size; MJPEG breaks ties).
            if (!haveBestEffort || m.fps > bestEffort.negotiatedFps ||
                (m.fps == bestEffort.negotiatedFps && isMjpeg && !bestEffort.useMjpeg)) {
                bestEffort = {cand.node, isMjpeg, m.fps, meets};
                haveBestEffort = true;
            }
        }
    }

    if (haveMjpegMeets) return mjpegMeets;   // 1. meets target, MJPEG
    if (haveRawMeets)   return rawMeets;     // 2. meets target, raw
    if (haveBestEffort) return bestEffort;   // 3. best effort at the size

    // 4. fallback: first candidate node, raw.
    V4l2Selection fallback;
    fallback.node = candidates.empty() ? std::string() : candidates.front().node;
    fallback.useMjpeg = false;
    fallback.negotiatedFps = reqFps;
    fallback.meetsTarget = false;
    return fallback;
}

}  // namespace moq2ts
```

- [ ] **Step 4: Wire the test target into CMake**

In `CMakeLists.txt`, add `src/media/V4l2Capabilities.cpp` to `SOURCES` (after `src/media/LibavCaptureSource.cpp`, line 141) and `src/media/V4l2Capabilities.h` to `HEADERS` (after `src/media/LibavCaptureSource.h`, line 158).

Then append, after the final `endif()` of the moqxr link block (end of file, after line 233):

```cmake
# Pure V4L2 mode-selection logic test (Linux only; no Qt/libav deps).
if(UNIX AND NOT APPLE)
    enable_testing()
    add_executable(moq2ts-v4l2-selection-tests
        tests/v4l2_selection_test.cpp
        src/media/V4l2Capabilities.cpp
    )
    target_include_directories(moq2ts-v4l2-selection-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME moq2ts-v4l2-selection-tests COMMAND moq2ts-v4l2-selection-tests)
endif()
```

- [ ] **Step 5: Build the test and verify it FAILS first (sanity), then PASSES**

Configure + build natively (the test has no Qt/libav deps, so a plain host build works):

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
cmake -S . -B build-test -DMOQ2TS_BUILD_WITH_MOCK_MOQXR=ON >/tmp/cfg.log 2>&1; echo "cfg=$?"
cmake --build build-test --target moq2ts-v4l2-selection-tests 2>&1 | tail -5
./build-test/moq2ts-v4l2-selection-tests; echo "test_exit=$?"
```
Expected: `test_exit=0`, no `FAIL:` lines. (If configuring the full app is heavy, this target only needs the two files listed; should compile quickly.)

- [ ] **Step 6: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/V4l2Capabilities.h src/media/V4l2Capabilities.cpp tests/v4l2_selection_test.cpp CMakeLists.txt
git commit -m "Add pure V4L2 mode selection with unit tests"
```

---

## Task 2: V4L2 ioctl probing + sysfs node grouping

Add the real device-query functions to `V4l2Capabilities.cpp`. These need real hardware to exercise fully (covered later by the live probe), so verification here is compile + a guarded smoke run.

**Files:**
- Modify: `src/media/V4l2Capabilities.cpp`

- [ ] **Step 1: Add includes and `queryModes`**

In `src/media/V4l2Capabilities.cpp`, add a Linux-guarded include block at the top (after `#include <algorithm>`):

```cpp
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <cstring>
#include <filesystem>
#endif
```

Then append, inside `namespace moq2ts` (before its closing brace):

```cpp
#if defined(__linux__)

std::vector<V4l2Mode> queryModes(const std::string& devNode) {
    std::vector<V4l2Mode> modes;
    const int fd = ::open(devNode.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return modes;
    }

    for (std::uint32_t fmtIndex = 0;; ++fmtIndex) {
        v4l2_fmtdesc fmtdesc{};
        fmtdesc.index = fmtIndex;
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (::ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
            break;  // no more formats
        }

        for (std::uint32_t sizeIndex = 0;; ++sizeIndex) {
            v4l2_frmsizeenum frmsize{};
            frmsize.index = sizeIndex;
            frmsize.pixel_format = fmtdesc.pixelformat;
            if (::ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) != 0) {
                break;
            }
            if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                continue;  // only discrete sizes are enumerated here
            }
            const int w = static_cast<int>(frmsize.discrete.width);
            const int h = static_cast<int>(frmsize.discrete.height);

            for (std::uint32_t ivalIndex = 0;; ++ivalIndex) {
                v4l2_frmivalenum frmival{};
                frmival.index = ivalIndex;
                frmival.pixel_format = fmtdesc.pixelformat;
                frmival.width = frmsize.discrete.width;
                frmival.height = frmsize.discrete.height;
                if (::ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) != 0) {
                    break;
                }
                if (frmival.type != V4L2_FRMIVAL_TYPE_DISCRETE) {
                    continue;
                }
                const double num = static_cast<double>(frmival.discrete.numerator);
                const double den = static_cast<double>(frmival.discrete.denominator);
                if (num <= 0.0) {
                    continue;
                }
                modes.push_back(V4l2Mode{fmtdesc.pixelformat, w, h, den / num});
            }
        }
    }

    ::close(fd);
    return modes;
}

#endif  // __linux__
```

- [ ] **Step 2: Add `groupNodesForCamera`**

Append, still inside the `#if defined(__linux__)` region (before its `#endif`):

```cpp
std::vector<std::string> groupNodesForCamera(const std::string& node) {
    namespace fs = std::filesystem;
    std::vector<std::string> group;

    // Resolve the USB parent of `node` via sysfs:
    //   /sys/class/video4linux/videoN/device -> <usb interface dir>
    // Sibling capture nodes share the same parent's parent (the USB device).
    const std::string leaf = fs::path(node).filename().string();  // "video2"
    std::error_code ec;
    const fs::path devLink =
        fs::path("/sys/class/video4linux") / leaf / "device";
    const fs::path parent = fs::canonical(devLink, ec);
    if (ec) {
        return {node};  // cannot resolve -> standalone
    }
    // The USB device dir is the interface dir's parent.
    const fs::path usbDevice = parent.parent_path();

    const fs::path classDir("/sys/class/video4linux");
    for (fs::directory_iterator it(classDir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string sibLeaf = it->path().filename().string();
        std::error_code ec2;
        const fs::path sibParent =
            fs::canonical(it->path() / "device", ec2);
        if (ec2) {
            continue;
        }
        if (sibParent.parent_path() == usbDevice) {
            group.push_back("/dev/" + sibLeaf);
        }
    }
    if (group.empty()) {
        group.push_back(node);
    }
    std::sort(group.begin(), group.end());
    return group;
}
```

- [ ] **Step 3: Build the app (compile check via Docker bookworm)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: ends with `Build complete. ...moq2ts-publisher-bookworm`, exit 0. (V4l2Capabilities.cpp is now compiled into the app via the Task 1 SOURCES change.)

- [ ] **Step 4: Smoke-probe this hardware (verify ioctls return real modes)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
cat > /tmp/probe_modes.cpp <<'EOF'
#include "media/V4l2Capabilities.h"
#include <cstdio>
int main(int argc, char** argv) {
    const std::string node = argc > 1 ? argv[1] : "/dev/video2";
    auto modes = moq2ts::queryModes(node);
    std::printf("%s: %zu modes\n", node.c_str(), modes.size());
    for (auto& m : modes) {
        std::printf("  fmt=%c%c%c%c %dx%d @ %.1ffps\n",
            (char)(m.pixelFormat & 0xff), (char)((m.pixelFormat>>8)&0xff),
            (char)((m.pixelFormat>>16)&0xff), (char)((m.pixelFormat>>24)&0xff),
            m.width, m.height, m.fps);
    }
    auto group = moq2ts::groupNodesForCamera(node);
    std::printf("group: ");
    for (auto& g : group) std::printf("%s ", g.c_str());
    std::printf("\n");
    return 0;
}
EOF
g++ -std=c++20 -I src /tmp/probe_modes.cpp src/media/V4l2Capabilities.cpp -o /tmp/probe_modes
/tmp/probe_modes /dev/video2
```
Expected: lists MJPG 1920x1080@30 (and others) for `/dev/video2`, and `group:` includes both `/dev/video0` and `/dev/video2`. If the group only shows one node, note it — grouping fell back to standalone (still correct behavior, just less merged).

- [ ] **Step 5: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/V4l2Capabilities.cpp
git commit -m "Add V4L2 ioctl mode probing and sysfs node grouping"
```

---

## Task 3: Camera grouping in enumeration

Replace the `index==0` filter so the MJPG-capable node is reachable and cameras show as one entry carrying all their capture nodes.

**Files:**
- Modify: `src/media/LibavCaptureSource.h`
- Modify: `src/media/LibavCaptureSource.cpp`

- [ ] **Step 1: Add `candidateNodes` to `CaptureDevice`**

In `src/media/LibavCaptureSource.h`, change the struct (line 18):

```cpp
struct CaptureDevice {
    QString id;                 // stable node for settings persistence
    QString description;
    QStringList candidateNodes; // all capture nodes for this physical camera
};
```

Add `#include <QStringList>` near the other Qt includes (after `#include <QString>`, line 6).

- [ ] **Step 2: Include the capability header in the .cpp**

In `src/media/LibavCaptureSource.cpp`, add near the existing includes (after the `#include "MsftsMuxer.h"` / `M2tsPacketizer.h` group, or alongside the other `src/media` includes):

```cpp
#include "V4l2Capabilities.h"
```

- [ ] **Step 3: Replace the enumeration filter with grouping**

In `src/media/LibavCaptureSource.cpp`, `enumerateVideoInputs()` currently loops nodes and skips `if (!isPrimaryV4l2Device(deviceName)) continue;` then appends one device per node (around lines 983-1001). Replace that loop body so it groups by camera and keeps only nodes with real capture modes. Replace from `for (int i = 0; i < list->nb_devices; ++i) {` through the closing `}` of that loop with:

```cpp
    QSet<QString> claimedNodes;  // nodes already folded into a camera group
    for (int i = 0; i < list->nb_devices; ++i) {
        AVDeviceInfo* info = list->devices[i];
        if (!info) continue;
        if (!info->device_name || !*info->device_name) continue;
        const QString deviceName = QString::fromUtf8(info->device_name);

#if defined(Q_OS_LINUX)
        if (claimedNodes.contains(deviceName)) {
            continue;  // already represented by an earlier camera group
        }
        // Group all capture nodes for this physical camera; keep only those
        // that actually report capture modes (drops metadata-only nodes).
        const std::vector<std::string> group =
            groupNodesForCamera(deviceName.toStdString());
        QStringList capable;
        for (const std::string& n : group) {
            const QString qn = QString::fromStdString(n);
            claimedNodes.insert(qn);
            if (!queryModes(n).empty()) {
                capable.append(qn);
            }
        }
        if (capable.isEmpty()) {
            continue;  // no usable capture node in this group
        }
        capable.sort();
        CaptureDevice d;
        d.id = capable.first();                  // stable anchor for QSettings
        d.description = QString::fromUtf8(info->device_description
                                          ? info->device_description : info->device_name);
        d.candidateNodes = capable;
        devices.append(d);
#else
        bool isVideo = false;
        for (int k = 0; k < info->nb_media_types; ++k) {
            if (info->media_types[k] == AVMEDIA_TYPE_VIDEO) { isVideo = true; break; }
        }
        if (info->nb_media_types == 0) {
            isVideo = true;
        }
        if (!isVideo) continue;
        CaptureDevice d;
        d.id = deviceName;
        d.description = QString::fromUtf8(info->device_description
                                          ? info->device_description : info->device_name);
        d.candidateNodes = QStringList{deviceName};
        devices.append(d);
#endif
    }
```

Add `#include <QSet>` near the Qt includes at the top of the file if not already present.

- [ ] **Step 4: Remove the now-unused `isPrimaryV4l2Device`**

Delete the `isPrimaryV4l2Device` function (lines 106-123). It is no longer referenced. Verify:

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
grep -n "isPrimaryV4l2Device" src/media/LibavCaptureSource.cpp || echo "removed cleanly"
```
Expected: `removed cleanly`.

- [ ] **Step 5: Build (Docker bookworm)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...`, exit 0.

- [ ] **Step 6: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/LibavCaptureSource.h src/media/LibavCaptureSource.cpp
git commit -m "Group V4L2 capture nodes per camera in enumeration"
```

---

## Task 4: Best-mode selection + MJPEG open + rate reconciliation in addVideo

Wire the probing into `addVideo`: pick the node/format, open with `input_format=mjpeg` when MJPG wins, retry raw on MJPG failure, and reconcile the encoder rate to the negotiated input rate.

**Files:**
- Modify: `src/media/LibavCaptureSource.cpp`

- [ ] **Step 1: Select the node/format before opening**

In `addVideo` (`LibavCaptureSource.cpp:321`), the current head is:

```cpp
    bool addVideo(QString* error) {
        auto stream = std::make_unique<StreamState>();
        stream->video = true;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "framerate", QByteArray::number(config.videoFramerate).constData(), 0);
        av_dict_set(&options, "video_size", QStringLiteral("%1x%2").arg(config.videoWidth).arg(config.videoHeight).toUtf8().constData(), 0);
        if (!openInput(videoInputFormatName(), videoInputName(config.cameraDeviceId), AVMEDIA_TYPE_VIDEO, stream.get(), &options, error)) {
            av_dict_free(&options);
            return false;
        }
        av_dict_free(&options);
```

Replace it with selection + MJPEG-with-raw-retry. On non-Linux, `selectBestMode` over a single trivially-empty candidate yields the configured node with raw — preserving current behavior.

```cpp
    bool addVideo(QString* error) {
        auto stream = std::make_unique<StreamState>();
        stream->video = true;

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

        const auto openWith = [&](bool mjpeg, QString* openError) -> bool {
            AVDictionary* options = nullptr;
            av_dict_set(&options, "framerate", QByteArray::number(config.videoFramerate).constData(), 0);
            av_dict_set(&options, "video_size",
                        QStringLiteral("%1x%2").arg(config.videoWidth).arg(config.videoHeight).toUtf8().constData(), 0);
            if (mjpeg) {
                av_dict_set(&options, "input_format", "mjpeg", 0);
            }
            const bool opened = openInput(videoInputFormatName(),
                                          videoInputName(chosenNode),
                                          AVMEDIA_TYPE_VIDEO, stream.get(), &options, openError);
            av_dict_free(&options);
            return opened;
        };

        QString openError;
        if (!openWith(useMjpeg, &openError)) {
            // If MJPEG open failed, retry once with raw before giving up.
            if (useMjpeg && openWith(false, error)) {
                std::fprintf(stderr, "[moqxr][capture] MJPEG open failed (%s); fell back to raw\n",
                             openError.toUtf8().constData());
            } else {
                if (error && error->isEmpty()) {
                    *error = openError;
                }
                return false;
            }
        }
```

(The function continues with the existing encoder-setup code that begins `const AVCodec* encoder = avcodec_find_encoder_by_name("libopenh264");`.)

Add `#include <cstdio>` at the top of the file if not already present (the keyframe diag already uses `std::fprintf`, so it is present).

- [ ] **Step 2: Reconcile the encoder rate to the negotiated input rate**

Still in `addVideo`, the encoder rate is currently hard-set (lines 349-359):

```cpp
        stream->encoder->time_base = AVRational{1, config.videoFramerate};
        stream->encoder->framerate = AVRational{config.videoFramerate, 1};
        ...
        const int frameRate = std::max(1, config.videoFramerate);
        int gopSize = static_cast<int>(
            (static_cast<long long>(frameRate) * std::max(1, config.keyframeIntervalMs) + 500) / 1000);
```

Replace the rate derivation so it uses the negotiated input rate. Insert this just before `stream->encoder->time_base = ...` and use `negFps` in place of `config.videoFramerate` for the encoder rate and GOP math:

```cpp
        // Derive the encoder rate from what the device actually negotiated, so
        // timestamps and GOP length match reality even when the driver could not
        // honor the requested rate (e.g. a raw mode capped below target).
        AVRational negRate = stream->inputStream->avg_frame_rate;
        if (negRate.num <= 0 || negRate.den <= 0) {
            negRate = stream->inputStream->r_frame_rate;
        }
        int negFps = (negRate.num > 0 && negRate.den > 0)
                         ? std::max(1, static_cast<int>(std::lround(av_q2d(negRate))))
                         : std::max(1, config.videoFramerate);
        if (negFps != config.videoFramerate) {
            std::fprintf(stderr,
                         "[moqxr][capture] requested %d fps, capturing at %d fps\n",
                         config.videoFramerate, negFps);
        }

        stream->encoder->time_base = AVRational{1, negFps};
        stream->encoder->framerate = AVRational{negFps, 1};
```

And change the GOP lines to use `negFps`:

```cpp
        const int frameRate = std::max(1, negFps);
        int gopSize = static_cast<int>(
            (static_cast<long long>(frameRate) * std::max(1, config.keyframeIntervalMs) + 500) / 1000);
```

Add `#include <cmath>` at the top of the file if not present (for `std::lround`).

- [ ] **Step 3: Build (Docker bookworm)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...`, exit 0.

- [ ] **Step 4: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/LibavCaptureSource.cpp
git commit -m "Select MJPEG node and reconcile encoder rate in capture"
```

---

## Task 5: Structural guards + verification

**Files:**
- Create: `tests/v4l2-capability-probe-guards.sh`

- [ ] **Step 1: Write the guard script**

Create `tests/v4l2-capability-probe-guards.sh` (same idiom as `tests/keyframe-aligned-groups-guards.sh`: a `fail()` that exits non-zero):

```bash
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CAPS_HDR="$REPO_ROOT/src/media/V4l2Capabilities.h"
CAPS_CPP="$REPO_ROOT/src/media/V4l2Capabilities.cpp"
CAPTURE="$REPO_ROOT/src/media/LibavCaptureSource.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

grep -q 'V4l2Selection selectBestMode' "$CAPS_HDR" \
  || fail "V4l2Capabilities must declare selectBestMode"

grep -q 'kV4l2PixFmtMjpeg' "$CAPS_CPP" \
  || fail "selectBestMode must consider the MJPEG pixel format"

grep -q 'VIDIOC_ENUM_FRAMEINTERVALS' "$CAPS_CPP" \
  || fail "queryModes must enumerate frame intervals via ioctl"

grep -q 'groupNodesForCamera' "$CAPS_CPP" \
  || fail "V4l2Capabilities must group nodes per camera"

# The index==0 primary-device filter must be gone.
grep -q 'isPrimaryV4l2Device' "$CAPTURE" \
  && fail "isPrimaryV4l2Device filter must be removed from enumeration" || true

grep -q 'input_format' "$CAPTURE" \
  || fail "addVideo must request an input_format (mjpeg) when selected"

grep -q 'av_q2d(negRate)' "$CAPTURE" \
  || fail "addVideo must reconcile the encoder rate from the negotiated input rate"

# The probe path must be Linux-guarded.
grep -q '#if defined(__linux__)' "$CAPS_CPP" \
  || fail "queryModes/groupNodesForCamera must be guarded for Linux"

printf 'v4l2-capability-probe guards passed\n'
```

- [ ] **Step 2: Run the new guard + the full guard suite**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
chmod +x tests/v4l2-capability-probe-guards.sh
bash tests/v4l2-capability-probe-guards.sh
for t in tests/*.sh; do
  out=$(bash "$t" 2>&1); rc=$?
  printf "%-50s %s\n" "$(basename "$t")" "$([ $rc -eq 0 ] && echo PASS || echo "FAIL(rc=$rc)")"
done
```
Expected: new guard prints `v4l2-capability-probe guards passed`; every script in the suite prints `PASS`.

- [ ] **Step 3: Run the selection unit test via the native build**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
cmake --build build-test --target moq2ts-v4l2-selection-tests 2>&1 | tail -3
./build-test/moq2ts-v4l2-selection-tests; echo "test_exit=$?"
```
Expected: `test_exit=0`, no `FAIL:` lines.

- [ ] **Step 4: Live probe selection on this hardware**

Reuse the Task 2 probe, extended to print the selection the app would make:

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
cat > /tmp/probe_select.cpp <<'EOF'
#include "media/V4l2Capabilities.h"
#include <cstdio>
int main() {
    auto group = moq2ts::groupNodesForCamera("/dev/video0");
    std::vector<moq2ts::V4l2NodeModes> cands;
    for (auto& n : group) cands.push_back({n, moq2ts::queryModes(n)});
    auto s = moq2ts::selectBestMode(cands, 1920, 1080, 30.0);
    std::printf("group size=%zu chosen=%s mjpeg=%d fps=%.1f meets=%d\n",
        cands.size(), s.node.c_str(), s.useMjpeg, s.negotiatedFps, s.meetsTarget);
    return 0;
}
EOF
g++ -std=c++20 -I src /tmp/probe_select.cpp src/media/V4l2Capabilities.cpp -o /tmp/probe_select
/tmp/probe_select
```
Expected on this hardware: `group size=2 chosen=/dev/video2 mjpeg=1 fps=30.0 meets=1`. (If `group size=1`, grouping fell back to standalone — acceptable, but then selection only sees video0 and cannot reach MJPG; investigate the sysfs parent resolution before the manual run.)

- [ ] **Step 5: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add tests/v4l2-capability-probe-guards.sh
git commit -m "Add V4L2 capability-probe structural guards"
```

---

## Task 6: Manual acceptance verification

**Files:** none (manual run; user-performed).

- [ ] **Step 1: Confirm the app build is current**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...`, exit 0.

- [ ] **Step 2: Run a live publish at 1080p (ask the user to perform this)**

Launch the bookworm publisher, select the camera + a microphone, leave resolution at 1920x1080 / framerate 30, publish, let it run ~10s, capture stderr.

Success criteria:
- The `[video4linux2,v4l2] The driver changed the time per frame from 1/30 to 1/5` line is **gone** from stderr.
- A `[moqxr][capture] selected node=/dev/video2 mjpeg=1 ... meets=1` line appears.
- The encoder summary (`libx264 frame I:/P:`) is consistent with ~30fps over the run (e.g. ~300 frames in ~10s, not ~50).
- The keyframe-group diag shows boundaries ~1s apart (not ~6s), since GOP now uses the real 30fps.

- [ ] **Step 3: Report results**

Summarize: selection line, observed fps, GOP cadence, and whether playback/relay behavior improved. If the driver-downgrade line still appears or `mjpeg=0`, report it — that means selection did not pick the MJPG node and needs investigation (likely grouping or a resolution mismatch).

---

## Notes for the implementer

- This is a single repo (moq2ts); commit all tasks here. moqxr is untouched by this plan.
- Do not add Claude author taglines / "Generated with" / "Co-Authored-By" to commits (project convention).
- Prefer `import`/include of the new header over duplicating types; `selectBestMode` and its structs must stay free of Qt and libav so the unit test links only `V4l2Capabilities.cpp`.
- The `build-test` directory (Task 1 Step 5) is a throwaway native build for the unit test; the real app build remains the Docker bookworm script. If `build-test` configure pulls in Qt/libav heavily, the selection-test target still only needs `tests/v4l2_selection_test.cpp` + `src/media/V4l2Capabilities.cpp` — build just that target.
- macOS path: `enumerateVideoInputs` already returns early via `macEnumerateVideoInputs()` before the Linux block; ensure that early return still sets `candidateNodes` (the mac enumerator builds `CaptureDevice` directly — set `candidateNodes = {id}` there if it does not, so downstream code never sees an empty list). Verify during Task 3.
- `/tmp/probe_*.cpp` helpers are throwaway diagnostics; do not commit them.