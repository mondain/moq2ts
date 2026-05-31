#pragma once

#include <QByteArray>
#include <QString>

namespace moq2ts {

struct MsftsCatalog {
    QString track;
    int packetSize = 188;
    int packetsPerObject = 7;
    int programNumber = 1;
    int pmtPid = -1;
    int pcrPid = -1;
    // Per MSFTS Section 6.9, m2tsTimestampMode applies only to 192-octet source
    // packets ("arrival-time" or "opaque") and MUST NOT be present for 188.
    QString timestampMode;
    QByteArray initData;
    QString timelineTrack;

    // MSF common track/root fields (draft-ietf-moq-msf-00).
    bool isLive = true;
    int targetLatencyMs = 1000;
    QString role = QStringLiteral("video");
    QString mimeType = QStringLiteral("video/mp2t");
    qint64 bitrateBps = 0;
    qint64 generatedAtMs = 0;
};

class MsftsMuxer {
public:
    static QByteArray catalogJson(const MsftsCatalog& catalog);
};

} // namespace moq2ts
