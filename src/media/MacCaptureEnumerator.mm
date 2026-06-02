#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <QList>
#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "LibavCaptureSource.h"

namespace moq2ts {

static void requestAccessSync(AVMediaType mediaType) {
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:mediaType];
    if (status == AVAuthorizationStatusAuthorized || status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
        return;
    }
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:mediaType completionHandler:^(BOOL) {
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
}

static QList<CaptureDevice> enumerate(AVMediaType mediaType) {
    requestAccessSync(mediaType);
    QList<CaptureDevice> devices;

    NSArray<AVCaptureDeviceType>* types = nil;
    if (mediaType == AVMediaTypeVideo) {
        if (@available(macOS 14.0, *)) {
            types = @[
                AVCaptureDeviceTypeBuiltInWideAngleCamera,
                AVCaptureDeviceTypeExternal,
                AVCaptureDeviceTypeContinuityCamera,
                AVCaptureDeviceTypeDeskViewCamera,
            ];
        } else if (@available(macOS 10.15, *)) {
            types = @[
                AVCaptureDeviceTypeBuiltInWideAngleCamera,
                AVCaptureDeviceTypeExternalUnknown,
            ];
        }
    } else {
        if (@available(macOS 14.0, *)) {
            types = @[
                AVCaptureDeviceTypeMicrophone,
                AVCaptureDeviceTypeExternal,
            ];
        } else if (@available(macOS 10.15, *)) {
            types = @[
                AVCaptureDeviceTypeBuiltInMicrophone,
                AVCaptureDeviceTypeExternalUnknown,
            ];
        }
    }

    NSArray<AVCaptureDevice*>* deviceList = nil;
    if (types && [AVCaptureDeviceDiscoverySession class]) {
        AVCaptureDeviceDiscoverySession* session =
            [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                   mediaType:mediaType
                                                                    position:AVCaptureDevicePositionUnspecified];
        deviceList = session.devices;
    }
    if (!deviceList || deviceList.count == 0) {
        // Fallback to legacy API; deprecated but still functional on older macOS.
        deviceList = [AVCaptureDevice devicesWithMediaType:mediaType];
    }

    int index = 0;
    for (AVCaptureDevice* dev in deviceList) {
        CaptureDevice entry;
        entry.id = QString::number(index);
        NSString* name = dev.localizedName ?: dev.uniqueID;
        entry.description = QString::fromNSString(name);
        if (mediaType == AVMediaTypeVideo) {
            // Downstream code expects a non-empty candidate list for video; the
            // macOS path has a single node per device (its avfoundation index).
            entry.candidateNodes = QStringList{entry.id};
        }
        devices.append(entry);
        ++index;
    }
    return devices;
}

QList<CaptureDevice> macEnumerateVideoInputs() {
    return enumerate(AVMediaTypeVideo);
}

QList<CaptureDevice> macEnumerateAudioInputs() {
    return enumerate(AVMediaTypeAudio);
}

static AVCaptureDevice* deviceAtIndex(int wanted) {
    requestAccessSync(AVMediaTypeVideo);
    NSArray<AVCaptureDeviceType>* types = nil;
    if (@available(macOS 14.0, *)) {
        types = @[
            AVCaptureDeviceTypeBuiltInWideAngleCamera,
            AVCaptureDeviceTypeExternal,
            AVCaptureDeviceTypeContinuityCamera,
            AVCaptureDeviceTypeDeskViewCamera,
        ];
    } else if (@available(macOS 10.15, *)) {
        types = @[
            AVCaptureDeviceTypeBuiltInWideAngleCamera,
            AVCaptureDeviceTypeExternalUnknown,
        ];
    }
    NSArray<AVCaptureDevice*>* deviceList = nil;
    if (types && [AVCaptureDeviceDiscoverySession class]) {
        AVCaptureDeviceDiscoverySession* session =
            [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                   mediaType:AVMediaTypeVideo
                                                                    position:AVCaptureDevicePositionUnspecified];
        deviceList = session.devices;
    }
    if (!deviceList || deviceList.count == 0) {
        deviceList = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
    }
    if (wanted < 0 || static_cast<NSUInteger>(wanted) >= deviceList.count) {
        return nil;
    }
    return deviceList[wanted];
}

std::optional<VideoCaptureMode> macSelectBestVideoMode(const QString& deviceId,
                                                      int wantWidth,
                                                      int wantHeight,
                                                      int wantFps) {
    bool ok = false;
    int idx = deviceId.toInt(&ok);
    if (!ok) {
        return std::nullopt;
    }
    AVCaptureDevice* dev = deviceAtIndex(idx);
    if (!dev || dev.formats.count == 0) {
        return std::nullopt;
    }

    // Score each candidate so that we prefer modes with a usable framerate first
    // (low-fps modes like 1280x800@10fps cause avfoundation to hang on startup
    // on some cameras), then maximize pixels up to the requested size, then
    // tie-break toward MJPEG (cheaper to decode than raw scaling).
    constexpr double kMinUsableFps = 15.0;
    const long wantPixels = static_cast<long>(wantWidth) * static_cast<long>(wantHeight);
    std::optional<VideoCaptureMode> best;
    double bestScore = -std::numeric_limits<double>::infinity();

    for (AVCaptureDeviceFormat* fmt in dev.formats) {
        CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
        if (dim.width <= 0 || dim.height <= 0) {
            continue;
        }
        FourCharCode subType = CMFormatDescriptionGetMediaSubType(fmt.formatDescription);
        const bool isMjpeg = (subType == kCMVideoCodecType_JPEG)
                             || (subType == kCMVideoCodecType_JPEG_OpenDML);
        double maxFps = 0.0;
        for (AVFrameRateRange* range in fmt.videoSupportedFrameRateRanges) {
            maxFps = std::max(maxFps, static_cast<double>(range.maxFrameRate));
        }
        if (maxFps <= 0.0) {
            continue;
        }
        const double fps = std::min(static_cast<double>(wantFps), maxFps);

        const long pixels = static_cast<long>(dim.width) * dim.height;
        // Score components (higher is better):
        //   - usableFps: huge bonus if fps >= 15, else penalty proportional to gap
        //   - sizeFit:   prefer pixels closer to wantPixels without exceeding it;
        //                modes larger than wanted get a smaller bonus
        //   - mjpegBias: tie-break MJPEG over raw
        const double usableFps = (maxFps >= kMinUsableFps) ? 1.0e9 : (maxFps - kMinUsableFps) * 1.0e6;
        const double sizeFit = (pixels <= wantPixels)
                                   ? static_cast<double>(pixels)
                                   : static_cast<double>(wantPixels) -
                                         static_cast<double>(pixels - wantPixels);
        const double mjpegBias = isMjpeg ? 1.0 : 0.0;
        const double score = usableFps + sizeFit + mjpegBias;
        if (score > bestScore) {
            bestScore = score;
            best = VideoCaptureMode{dim.width, dim.height, fps, isMjpeg};
        }
    }
    return best;
}

} // namespace moq2ts

#endif
