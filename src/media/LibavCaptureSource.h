#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "../app/PublishConfig.h"
#include "M2tsPacketizer.h"

namespace moq2ts {

struct CaptureDevice {
    QString id;                 // stable node for settings persistence
    QString description;
    QStringList candidateNodes; // all capture nodes for this physical camera
};

struct VideoCaptureMode {
    int width = 0;
    int height = 0;
    double framerate = 0.0;
    bool mjpeg = false;  // chosen format is MJPEG-encoded
};

#ifdef __APPLE__
// Returns the supported video mode closest to (wantWidth, wantHeight, wantFps) for
// the AVFoundation device with the given numeric id, or std::nullopt if the device
// is unknown / has no listable formats.
std::optional<VideoCaptureMode> macSelectBestVideoMode(const QString& deviceId,
                                                      int wantWidth,
                                                      int wantHeight,
                                                      int wantFps);
#endif

class LibavCaptureSource final {
public:
    explicit LibavCaptureSource(PublishConfig config);
    ~LibavCaptureSource();

    static QList<CaptureDevice> enumerateVideoInputs();
    static QList<CaptureDevice> enumerateAudioInputs();

    bool open(QString* error);
    bool readObject(int packetsPerObject, M2tsObject* object, std::atomic<bool>& running, QString* error);
    void setPreviewCallbacks(std::function<void(const QImage&)> videoFrameCallback,
                             std::function<void(double, double)> audioLevelsCallback);

    int packetSize() const;
    int programNumber() const;
    int pmtPid() const;
    int pcrPid() const;
    QByteArray initData() const;
    bool randomAccessActive() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace moq2ts
