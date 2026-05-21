#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>

#include <QList>
#include <QString>

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

} // namespace moq2ts

#endif
