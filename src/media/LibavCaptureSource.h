#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>

#include "../app/PublishConfig.h"
#include "M2tsPacketizer.h"

namespace moq2ts {

class LibavCaptureSource final {
public:
    explicit LibavCaptureSource(PublishConfig config);
    ~LibavCaptureSource();

    bool open(QString* error);
    bool readObject(int packetsPerObject, M2tsObject* object, std::atomic<bool>& running, QString* error);

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
