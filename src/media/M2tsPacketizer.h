#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstdint>
#include <set>
#include <utility>

namespace moq2ts {

struct M2tsObject {
    QByteArray payload;
    std::uint64_t groupId = 0;
    std::uint64_t objectId = 0;
};

class M2tsPacketizer final {
public:
    explicit M2tsPacketizer(QString sourcePath);

    bool open(int requestedProgramNumber, QString* error);
    bool readObject(int packetsPerObject, M2tsObject* object, QString* error);
    int packetSize() const;
    int programNumber() const;
    int pmtPid() const;
    int pcrPid() const;
    QByteArray initData() const;
    std::uint64_t objectsRead() const;

    // Probes the file with libav and returns its duration in integer
    // milliseconds, or 0 if unknown / libav unavailable / on any failure.
    static qint64 probeDurationMs(const QString& sourcePath);

private:
    bool detectPacketSize(QString* error);
    bool collectInitData(QString* error);
    bool packetHasSync(const QByteArray& packet) const;
    QByteArray tsPacketView(const QByteArray& sourcePacket) const;

    QString m_sourcePath;
    QFile m_file;
    int m_packetSize = 0;
    int m_requestedProgramNumber = 0;
    int m_programNumber = 1;
    int m_pmtPid = -1;
    int m_pcrPid = -1;
    std::set<int> m_selectedPids;
    QByteArray m_initData;
    std::uint64_t m_nextObjectId = 0;
};

} // namespace moq2ts
