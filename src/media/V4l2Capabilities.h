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
