#include "media/V4l2Capabilities.h"

#include <cmath>

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
            // Use the same epsilon as meets-target so driver rounding (e.g.
            // 29.97 vs 30) does not defeat the MJPEG tiebreak.
            const bool higher = m.fps > bestEffort.negotiatedFps + kFpsEpsilon;
            const bool tie = std::abs(m.fps - bestEffort.negotiatedFps) <= kFpsEpsilon;
            if (!haveBestEffort || higher ||
                (tie && isMjpeg && !bestEffort.useMjpeg)) {
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
    fallback.negotiatedFps = 0.0;
    fallback.meetsTarget = false;
    return fallback;
}

}  // namespace moq2ts
