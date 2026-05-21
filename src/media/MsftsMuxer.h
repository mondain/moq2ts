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
    QString timestampMode = QStringLiteral("none");
    QByteArray initData;
};

class MsftsMuxer {
public:
    static QByteArray catalogJson(const MsftsCatalog& catalog);
};

} // namespace moq2ts
