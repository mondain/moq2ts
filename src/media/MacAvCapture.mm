#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

#include "MacAvCapture.h"
#include "LibavCaptureSource.h"

@class MoqAvVideoDelegate;
@class MoqAvAudioDelegate;

namespace moq2ts {

struct MacAvCapture::Impl {
    // Owning ObjC refs live in an Objective-C instance (held via
    // CFBridgingRetain in ownerRetained) — ObjC pointers in plain C++ structs
    // get treated as __unsafe_unretained under ARC.
    void* ownerRetained = nullptr;

    std::mutex frameMutex;
    std::condition_variable frameCv;
    std::optional<VideoFrame> latestFrame;

    std::mutex audioMutex;
    std::optional<AudioBuffer> latestAudio;

    bool running = false;
};

} // namespace moq2ts

@interface MoqAvSessionOwner : NSObject
@property (nonatomic, strong) AVCaptureSession* session;
@property (nonatomic, strong) AVCaptureDeviceInput* videoInput;
@property (nonatomic, strong) AVCaptureVideoDataOutput* videoOutput;
@property (nonatomic, strong) MoqAvVideoDelegate* videoDelegate;
@property (nonatomic, strong) dispatch_queue_t videoQueue;
@property (nonatomic, strong) AVCaptureDeviceInput* audioInput;
@property (nonatomic, strong) AVCaptureAudioDataOutput* audioOutput;
@property (nonatomic, strong) MoqAvAudioDelegate* audioDelegate;
@property (nonatomic, strong) dispatch_queue_t audioQueue;
@end

@implementation MoqAvSessionOwner
@end

@interface MoqAvVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    moq2ts::MacAvCapture::Impl* impl;
}
@end

@interface MoqAvAudioDelegate : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate> {
@public
    moq2ts::MacAvCapture::Impl* impl;
}
@end

@implementation MoqAvVideoDelegate
- (void)captureOutput:(AVCaptureOutput*)output
        didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    if (!impl) {
        return;
    }
    CVImageBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixelBuffer) {
        return;
    }
    OSType pixFmt = CVPixelBufferGetPixelFormatType(pixelBuffer);
    if (pixFmt != kCVPixelFormatType_32BGRA
        && pixFmt != kCVPixelFormatType_422YpCbCr8 /* 2vuy / uyvy422 */) {
        return;
    }
    if (CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return;
    }
    const size_t width = CVPixelBufferGetWidth(pixelBuffer);
    const size_t height = CVPixelBufferGetHeight(pixelBuffer);
    const size_t srcStride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    const uint8_t* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));

    moq2ts::MacAvCapture::VideoFrame frame;
    frame.width = static_cast<int>(width);
    frame.height = static_cast<int>(height);
    const size_t dstStride = width * 4;
    frame.bgra.resize(dstStride * height);
    if (pixFmt == kCVPixelFormatType_32BGRA) {
        if (srcStride == dstStride) {
            std::memcpy(frame.bgra.data(), src, frame.bgra.size());
        } else {
            for (size_t y = 0; y < height; ++y) {
                std::memcpy(frame.bgra.data() + y * dstStride, src + y * srcStride, dstStride);
            }
        }
    } else {
        // uyvy422 (2vuy): each 4 source bytes = U Y0 V Y1 → 2 destination
        // pixels. BT.601 limited-range conversion is good enough for a webcam
        // preview; the integer math avoids floating-point per-pixel.
        auto clip = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
        for (size_t y = 0; y < height; ++y) {
            const uint8_t* sRow = src + y * srcStride;
            uint8_t* dRow = frame.bgra.data() + y * dstStride;
            for (size_t x = 0; x + 1 < width; x += 2) {
                int u  = sRow[2 * x + 0] - 128;
                int y0 = sRow[2 * x + 1] - 16;
                int v  = sRow[2 * x + 2] - 128;
                int y1 = sRow[2 * x + 3] - 16;
                int c0 = 298 * y0;
                int c1 = 298 * y1;
                int r0 = (c0 + 409 * v + 128) >> 8;
                int g0 = (c0 - 100 * u - 208 * v + 128) >> 8;
                int b0 = (c0 + 516 * u + 128) >> 8;
                int r1 = (c1 + 409 * v + 128) >> 8;
                int g1 = (c1 - 100 * u - 208 * v + 128) >> 8;
                int b1 = (c1 + 516 * u + 128) >> 8;
                dRow[4 * x + 0] = clip(b0);
                dRow[4 * x + 1] = clip(g0);
                dRow[4 * x + 2] = clip(r0);
                dRow[4 * x + 3] = 0xff;
                dRow[4 * x + 4] = clip(b1);
                dRow[4 * x + 5] = clip(g1);
                dRow[4 * x + 6] = clip(r1);
                dRow[4 * x + 7] = 0xff;
            }
        }
    }
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    if (CMTIME_IS_VALID(pts)) {
        frame.ptsMicros = static_cast<int64_t>(CMTimeGetSeconds(pts) * 1.0e6);
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    {
        std::lock_guard<std::mutex> lk(impl->frameMutex);
        impl->latestFrame = std::move(frame);
    }
    impl->frameCv.notify_all();
}
@end

@implementation MoqAvAudioDelegate
- (void)captureOutput:(AVCaptureOutput*)output
        didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    if (!impl) {
        return;
    }
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!format) {
        return;
    }
    const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(format);
    if (!asbd) {
        return;
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!block) {
        return;
    }
    size_t totalLen = 0;
    char* data = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, nullptr, &totalLen, &data) != kCMBlockBufferNoErr || !data) {
        return;
    }

    moq2ts::MacAvCapture::AudioBuffer audio;
    audio.sampleRate = static_cast<int>(asbd->mSampleRate);
    audio.channels = static_cast<int>(asbd->mChannelsPerFrame);
    const int channels = std::max(1, audio.channels);
    const int bitsPerChannel = static_cast<int>(asbd->mBitsPerChannel);
    const bool isFloat = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool isPacked = (asbd->mFormatFlags & kAudioFormatFlagIsPacked) != 0;
    const bool isInterleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
    const int bytesPerSample = bitsPerChannel / 8;
    if (bytesPerSample <= 0) {
        return;
    }
    // For non-interleaved (planar) layouts each channel lives in its own
    // CMBlockBuffer entry; we use the AudioBufferList API to walk them.
    if (!isInterleaved) {
        AudioBufferList bufferList{};
        CMBlockBufferRef ownedBlock = nullptr;
        OSStatus rc = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, nullptr, &bufferList, sizeof(bufferList),
            kCFAllocatorDefault, kCFAllocatorDefault, 0, &ownedBlock);
        if (rc != noErr || bufferList.mNumberBuffers == 0) {
            if (ownedBlock) CFRelease(ownedBlock);
            return;
        }
        const UInt32 frames = bufferList.mBuffers[0].mDataByteSize / bytesPerSample;
        audio.samples.resize(static_cast<size_t>(frames) * channels);
        for (UInt32 ch = 0; ch < bufferList.mNumberBuffers && ch < (UInt32)channels; ++ch) {
            const auto* src = static_cast<const uint8_t*>(bufferList.mBuffers[ch].mData);
            for (UInt32 f = 0; f < frames; ++f) {
                float s = 0.0f;
                if (isFloat && bytesPerSample == 4) {
                    s = reinterpret_cast<const float*>(src)[f];
                } else if (!isFloat && bytesPerSample == 2) {
                    s = reinterpret_cast<const int16_t*>(src)[f] / 32768.0f;
                }
                audio.samples[f * channels + ch] = s;
            }
        }
        if (ownedBlock) CFRelease(ownedBlock);
    } else if (!isPacked) {
        return;
    } else {
        const size_t frames = totalLen / (bytesPerSample * channels);
        audio.samples.resize(frames * channels);
        if (isFloat && bytesPerSample == 4) {
            std::memcpy(audio.samples.data(), data, frames * channels * sizeof(float));
        } else if (!isFloat && bytesPerSample == 2) {
            const int16_t* src = reinterpret_cast<const int16_t*>(data);
            for (size_t i = 0; i < frames * static_cast<size_t>(channels); ++i) {
                audio.samples[i] = src[i] / 32768.0f;
            }
        } else {
            return;
        }
    }
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    if (CMTIME_IS_VALID(pts)) {
        audio.ptsMicros = static_cast<int64_t>(CMTimeGetSeconds(pts) * 1.0e6);
    }
    {
        std::lock_guard<std::mutex> lk(impl->audioMutex);
        impl->latestAudio = std::move(audio);
    }
}
@end

namespace moq2ts {

namespace {

AVCaptureDevice* findVideoDevice(int wantedIndex) {
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
    if (wantedIndex < 0 || static_cast<NSUInteger>(wantedIndex) >= deviceList.count) {
        return nil;
    }
    return deviceList[wantedIndex];
}

AVCaptureDevice* findAudioDevice(int wantedIndex) {
    NSArray<AVCaptureDeviceType>* types = nil;
    if (@available(macOS 14.0, *)) {
        types = @[AVCaptureDeviceTypeMicrophone, AVCaptureDeviceTypeExternal];
    } else if (@available(macOS 10.15, *)) {
        types = @[AVCaptureDeviceTypeBuiltInMicrophone, AVCaptureDeviceTypeExternalUnknown];
    }
    NSArray<AVCaptureDevice*>* deviceList = nil;
    if (types && [AVCaptureDeviceDiscoverySession class]) {
        AVCaptureDeviceDiscoverySession* session =
            [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                   mediaType:AVMediaTypeAudio
                                                                    position:AVCaptureDevicePositionUnspecified];
        deviceList = session.devices;
    }
    if (!deviceList || deviceList.count == 0) {
        deviceList = [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio];
    }
    if (wantedIndex < 0 || static_cast<NSUInteger>(wantedIndex) >= deviceList.count) {
        return nil;
    }
    return deviceList[wantedIndex];
}

} // namespace

MacAvCapture::MacAvCapture() : m_impl(std::make_unique<Impl>()) {}

MacAvCapture::~MacAvCapture() {
    stop();
}

bool MacAvCapture::startVideo(const QString& deviceId,
                              int wantWidth,
                              int wantHeight,
                              int wantFps,
                              QString* error) {
    if (m_impl->running) {
        return true;
    }

    bool ok = false;
    int idx = deviceId.toInt(&ok);
    if (!ok) {
        if (error) *error = QStringLiteral("Invalid camera device id '%1'").arg(deviceId);
        return false;
    }

    auto best = macSelectBestVideoMode(deviceId, wantWidth, wantHeight, wantFps);
    int useW = wantWidth;
    int useH = wantHeight;
    int useFps = wantFps;
    if (best) {
        useW = best->width;
        useH = best->height;
        useFps = std::max(1, static_cast<int>(std::lround(best->framerate)));
    }
    (void)useFps;

    __block MoqAvSessionOwner* owner = nil;
    __block QString localError;
    __block bool ok2 = false;
    moq2ts::MacAvCapture::Impl* implPtr = m_impl.get();

    // Build, configure and start AVCaptureSession on the main thread. Started
    // from a thread without a CFRunLoop (the Qt worker), the session reports
    // isRunning=YES but never invokes the delegate.
    dispatch_sync(dispatch_get_main_queue(), ^{
        AVCaptureDevice* dev = findVideoDevice(idx);
        if (!dev) {
            localError = QStringLiteral("Camera device index %1 not found").arg(idx);
            return;
        }
        NSError* nsErr = nil;
        owner = [[MoqAvSessionOwner alloc] init];
        owner.session = [[AVCaptureSession alloc] init];
        [owner.session beginConfiguration];

        NSString* preset = AVCaptureSessionPreset1280x720;
        if (useW <= 640 && useH <= 480 && [owner.session canSetSessionPreset:AVCaptureSessionPreset640x480]) {
            preset = AVCaptureSessionPreset640x480;
        } else if ([owner.session canSetSessionPreset:AVCaptureSessionPreset1280x720]) {
            preset = AVCaptureSessionPreset1280x720;
        }
        owner.session.sessionPreset = preset;

        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&nsErr];
        if (!input || ![owner.session canAddInput:input]) {
            localError = QStringLiteral("Cannot add camera input: %1").arg(QString::fromNSString(nsErr.localizedDescription ?: @"refused"));
            [owner.session commitConfiguration];
            owner = nil;
            return;
        }
        [owner.session addInput:input];
        owner.videoInput = input;

        AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
        output.alwaysDiscardsLateVideoFrames = YES;
        if (![owner.session canAddOutput:output]) {
            localError = QStringLiteral("Cannot add video output");
            [owner.session commitConfiguration];
            owner = nil;
            return;
        }
        [owner.session addOutput:output];

        // Leave videoSettings nil so AVFoundation hands us the camera's native
        // pixel format (uyvy422/yuyv422/nv12). Forcing BGRA caused the LifeCam
        // Cinema to deliver 0–1 frames and then stop pushing samples.
        MoqAvVideoDelegate* delegate = [[MoqAvVideoDelegate alloc] init];
        delegate->impl = implPtr;
        owner.videoQueue = dispatch_queue_create("org.moq2ts.video", DISPATCH_QUEUE_SERIAL);
        [output setSampleBufferDelegate:delegate queue:owner.videoQueue];
        owner.videoOutput = output;
        owner.videoDelegate = delegate;
        [owner.session commitConfiguration];

        [owner.session startRunning];
        ok2 = true;
    });

    if (!ok2) {
        if (error) *error = localError.isEmpty() ? QStringLiteral("Failed to start camera") : localError;
        return false;
    }
    m_impl->running = true;
    m_impl->ownerRetained = (void*)CFBridgingRetain(owner);

    // Some USB cameras (e.g. older LifeCams over USB 2) accept a 720p preset
    // and report isRunning=YES but never deliver a sample buffer. Wait briefly
    // for the first frame; if none arrives, tear down and retry at 480p.
    {
        std::unique_lock<std::mutex> lk(m_impl->frameMutex);
        m_impl->frameCv.wait_for(lk, std::chrono::milliseconds(1500), [&] {
            return m_impl->latestFrame.has_value() || !m_impl->running;
        });
        if (!m_impl->latestFrame && m_impl->running) {
            lk.unlock();
            stop();
            __block MoqAvSessionOwner* owner2 = nil;
            __block bool ok3 = false;
            moq2ts::MacAvCapture::Impl* implPtr2 = m_impl.get();
            dispatch_sync(dispatch_get_main_queue(), ^{
                AVCaptureDevice* dev = findVideoDevice(idx);
                if (!dev) return;
                NSError* nsErr = nil;
                owner2 = [[MoqAvSessionOwner alloc] init];
                owner2.session = [[AVCaptureSession alloc] init];
                [owner2.session beginConfiguration];
                owner2.session.sessionPreset = AVCaptureSessionPreset640x480;
                AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&nsErr];
                if (!input || ![owner2.session canAddInput:input]) {
                    [owner2.session commitConfiguration];
                    owner2 = nil;
                    return;
                }
                [owner2.session addInput:input];
                owner2.videoInput = input;

                AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
                output.alwaysDiscardsLateVideoFrames = YES;
                if (![owner2.session canAddOutput:output]) {
                    [owner2.session commitConfiguration];
                    owner2 = nil;
                    return;
                }
                [owner2.session addOutput:output];
                MoqAvVideoDelegate* delegate = [[MoqAvVideoDelegate alloc] init];
                delegate->impl = implPtr2;
                owner2.videoQueue = dispatch_queue_create("org.moq2ts.video", DISPATCH_QUEUE_SERIAL);
                [output setSampleBufferDelegate:delegate queue:owner2.videoQueue];
                owner2.videoOutput = output;
                owner2.videoDelegate = delegate;
                [owner2.session commitConfiguration];
                [owner2.session startRunning];
                ok3 = true;
            });
            if (!ok3) {
                if (error) *error = QStringLiteral("Camera produced no frames at 720p and fallback to 480p failed");
                return false;
            }
            m_impl->running = true;
            m_impl->ownerRetained = (void*)CFBridgingRetain(owner2);
        }
    }
    return true;
}

void MacAvCapture::stop() {
    if (!m_impl->running) {
        return;
    }
    m_impl->running = false;
    if (m_impl->ownerRetained) {
        MoqAvSessionOwner* owner = CFBridgingRelease(m_impl->ownerRetained);
        m_impl->ownerRetained = nullptr;
        // Tear down on whatever thread we're on; AVCaptureSession's stopRunning
        // is documented as safe off the main thread, and dispatching to main
        // here would deadlock against PreviewPanel::stopPreview's
        // QThread::wait() on the UI thread.
        [owner.session stopRunning];
        if (owner.videoOutput) {
            [owner.videoOutput setSampleBufferDelegate:nil queue:nullptr];
        }
        if (owner.videoDelegate) {
            owner.videoDelegate->impl = nullptr;
        }
        if (owner.audioOutput) {
            [owner.audioOutput setSampleBufferDelegate:nil queue:nullptr];
        }
        if (owner.audioDelegate) {
            owner.audioDelegate->impl = nullptr;
        }
    }
    m_impl->frameCv.notify_all();
}

bool MacAvCapture::startAudio(const QString& deviceId, QString* error) {
    if (!m_impl->running || !m_impl->ownerRetained) {
        if (error) *error = QStringLiteral("Audio requires a running capture session");
        return false;
    }
    bool ok = false;
    int idx = deviceId.toInt(&ok);
    if (!ok) {
        if (error) *error = QStringLiteral("Invalid microphone device id '%1'").arg(deviceId);
        return false;
    }

    // Request mic permission synchronously; without this AVCaptureDeviceInput
    // returns success but no samples flow.
    AVAuthorizationStatus auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (auth == AVAuthorizationStatusNotDetermined) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    }
    if (auth != AVAuthorizationStatusAuthorized) {
        if (error) *error = QStringLiteral("Microphone permission not granted (status=%1)").arg((int)auth);
        return false;
    }

    MoqAvSessionOwner* owner = (__bridge MoqAvSessionOwner*)m_impl->ownerRetained;
    moq2ts::MacAvCapture::Impl* implPtr = m_impl.get();
    __block bool ok2 = false;
    __block QString localError;

    dispatch_sync(dispatch_get_main_queue(), ^{
        AVCaptureDevice* dev = findAudioDevice(idx);
        if (!dev) {
            localError = QStringLiteral("Microphone device index %1 not found").arg(idx);
            return;
        }
        NSError* nsErr = nil;
        [owner.session beginConfiguration];
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&nsErr];
        if (!input || ![owner.session canAddInput:input]) {
            localError = QStringLiteral("Cannot add mic input: %1").arg(QString::fromNSString(nsErr.localizedDescription ?: @"refused"));
            [owner.session commitConfiguration];
            return;
        }
        [owner.session addInput:input];
        owner.audioInput = input;

        AVCaptureAudioDataOutput* output = [[AVCaptureAudioDataOutput alloc] init];
        if (![owner.session canAddOutput:output]) {
            localError = QStringLiteral("Cannot add audio output");
            [owner.session commitConfiguration];
            return;
        }
        [owner.session addOutput:output];
        MoqAvAudioDelegate* delegate = [[MoqAvAudioDelegate alloc] init];
        delegate->impl = implPtr;
        owner.audioQueue = dispatch_queue_create("org.moq2ts.audio", DISPATCH_QUEUE_SERIAL);
        [output setSampleBufferDelegate:delegate queue:owner.audioQueue];
        owner.audioOutput = output;
        owner.audioDelegate = delegate;
        [owner.session commitConfiguration];
        ok2 = true;
    });

    if (!ok2) {
        if (error) *error = localError.isEmpty() ? QStringLiteral("Failed to start microphone") : localError;
        return false;
    }
    return true;
}

bool MacAvCapture::nextAudioBuffer(AudioBuffer& out) {
    std::lock_guard<std::mutex> lk(m_impl->audioMutex);
    if (!m_impl->latestAudio) {
        return false;
    }
    out = std::move(*m_impl->latestAudio);
    m_impl->latestAudio.reset();
    return true;
}

bool MacAvCapture::nextVideoFrame(VideoFrame& out, int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_impl->frameMutex);
    if (!m_impl->latestFrame) {
        m_impl->frameCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
            return m_impl->latestFrame.has_value() || !m_impl->running;
        });
    }
    if (!m_impl->latestFrame) {
        return false;
    }
    out = std::move(*m_impl->latestFrame);
    m_impl->latestFrame.reset();
    return true;
}

} // namespace moq2ts

#endif
