#include "media/V4l2Capabilities.h"

#include <cstdint>
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
