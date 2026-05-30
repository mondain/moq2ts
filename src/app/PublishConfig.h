#pragma once

#include <QString>

namespace moq2ts {

enum class AudioCodecPreset {
    AAC,
    Opus,
};

struct PublishConfig {
    QString moqEndpoint = "mock://local";
    QString namespaceName = "live";
    QString streamName = "sample-stream";

    QString videoSource = QString(); // M2TS file/pipe/URL for H.264 source
    QString audioSource = QString(); // M2TS file/pipe/URL for audio source (optional)
    QString cameraDeviceId = QString(); // UI-selected capture device id
    QString microphoneDeviceId = QString(); // UI-selected capture device id

    int videoWidth = 1920;
    int videoHeight = 1080;
    int videoFramerate = 30;
    int videoTargetBitrateKbps = 2500;

    int audioSampleRate = 48000;
    int audioChannels = 2;
    int audioTargetBitrateKbps = 160;
    AudioCodecPreset audioCodec = AudioCodecPreset::AAC;

    int fragmentDurationMs = 250;
    int targetSegmentBytes = 64 * 1024;
    int programNumber = 0; // 0 selects the first nonzero PAT program

    bool forceRealtime = true;
    bool useOpenh264 = true;
    bool useLibAvTranscode = true;
    bool useLibOpusFallback = false;
};

} // namespace moq2ts
