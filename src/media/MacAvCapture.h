#pragma once

#ifdef __APPLE__

#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace moq2ts {

// Native AVFoundation capture for macOS. Bypasses libavdevice/avfoundation,
// which hangs in avformat_open_input on cameras driven from a non-main thread
// because it does not pump the AVCaptureSession's run loop. This class owns an
// AVCaptureSession running on a dedicated dispatch queue; the latest decoded
// frame is held in a single slot that nextVideoFrame() copies out (the previous
// frame is dropped if the consumer is slower than the device).
class MacAvCapture {
public:
    struct VideoFrame {
        int width = 0;
        int height = 0;
        // Tightly packed BGRA, top-down.
        std::vector<uint8_t> bgra;
        int64_t ptsMicros = 0;
    };

    struct AudioBuffer {
        int sampleRate = 0;
        int channels = 0;
        // Interleaved float32, [-1.0, 1.0]. Channel layout: [L, R, L, R, ...]
        // when channels >= 2; mono is just a single stream.
        std::vector<float> samples;
        int64_t ptsMicros = 0;
    };

    MacAvCapture();
    ~MacAvCapture();
    MacAvCapture(const MacAvCapture&) = delete;
    MacAvCapture& operator=(const MacAvCapture&) = delete;

    // deviceId is the numeric index produced by MacCaptureEnumerator (matches
    // the avfoundation index convention). Returns false and fills *error on
    // any setup failure.
    bool startVideo(const QString& deviceId,
                    int wantWidth,
                    int wantHeight,
                    int wantFps,
                    QString* error);

    // Add an audio input to the same AVCaptureSession (must be called after
    // startVideo, or as the sole input on a video-less session).
    bool startAudio(const QString& deviceId, QString* error);
    void stop();

    // Blocks up to timeoutMs for a new frame. Returns false on timeout or if
    // the session is stopped. Always reads the most recent frame and discards
    // older ones, matching the preview's display-latest semantics.
    bool nextVideoFrame(VideoFrame& out, int timeoutMs);

    // Non-blocking pull of the most recent audio buffer; returns false when no
    // new audio has been delivered since the last call. Audio meters don't
    // need every sample, only periodic levels.
    bool nextAudioBuffer(AudioBuffer& out);

    struct Impl;
private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace moq2ts

#endif
