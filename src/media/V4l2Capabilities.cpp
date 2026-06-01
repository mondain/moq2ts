#include "media/V4l2Capabilities.h"

#include <algorithm>
#include <cmath>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <cstring>
#include <filesystem>
#endif

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

#endif  // __linux__

}  // namespace moq2ts
