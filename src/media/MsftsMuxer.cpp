#include "MsftsMuxer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace moq2ts {

QByteArray MsftsMuxer::catalogJson(const MsftsCatalog& catalog) {
    QJsonObject track;
    track.insert(QStringLiteral("name"), catalog.track);
    track.insert(QStringLiteral("packaging"), QStringLiteral("m2ts"));
    track.insert(QStringLiteral("m2tsPacketSize"), catalog.packetSize);
    track.insert(QStringLiteral("m2tsPacketsPerObject"), catalog.packetsPerObject);
    track.insert(QStringLiteral("m2tsProgramNumber"), catalog.programNumber);
    if (catalog.pmtPid >= 0) {
        track.insert(QStringLiteral("m2tsPmtPid"), catalog.pmtPid);
    }
    if (catalog.pcrPid >= 0) {
        track.insert(QStringLiteral("m2tsPcrPid"), catalog.pcrPid);
    }
    track.insert(QStringLiteral("m2tsTimestampMode"), catalog.timestampMode);
    if (!catalog.initData.isEmpty()) {
        track.insert(QStringLiteral("initData"), QString::fromLatin1(catalog.initData.toBase64()));
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("format"), QStringLiteral("msf"));
    root.insert(QStringLiteral("tracks"), QJsonArray{track});
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace moq2ts
