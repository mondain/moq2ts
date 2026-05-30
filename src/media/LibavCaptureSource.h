#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "../app/PublishConfig.h"
#include "M2tsPacketizer.h"

namespace moq2ts {

struct CaptureDevice {
    QString id;
    QString description;
};

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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace moq2ts
