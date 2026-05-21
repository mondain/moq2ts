#include "MsftsMuxer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace moq2ts {

QByteArray MsftsMuxer::catalogJson(const MsftsCatalog& catalog) {
    QJsonObject mediaTrack;
    mediaTrack.insert(QStringLiteral("name"), catalog.track);
    mediaTrack.insert(QStringLiteral("packaging"), QStringLiteral("m2ts"));
    mediaTrack.insert(QStringLiteral("m2tsPacketSize"), catalog.packetSize);
    mediaTrack.insert(QStringLiteral("m2tsPacketsPerObject"), catalog.packetsPerObject);
    mediaTrack.insert(QStringLiteral("m2tsProgramNumber"), catalog.programNumber);
    if (catalog.pmtPid >= 0) {
        mediaTrack.insert(QStringLiteral("m2tsPmtPid"), catalog.pmtPid);
    }
    if (catalog.pcrPid >= 0) {
        mediaTrack.insert(QStringLiteral("m2tsPcrPid"), catalog.pcrPid);
    }
    mediaTrack.insert(QStringLiteral("m2tsTimestampMode"), catalog.timestampMode);
    if (!catalog.initData.isEmpty()) {
        mediaTrack.insert(QStringLiteral("initData"), QString::fromLatin1(catalog.initData.toBase64()));
    }

    QJsonArray tracks;
    tracks.append(mediaTrack);

    if (!catalog.timelineTrack.isEmpty()) {
        QJsonObject timelineTrack;
        timelineTrack.insert(QStringLiteral("name"), catalog.timelineTrack);
        timelineTrack.insert(QStringLiteral("packaging"), QStringLiteral("msf-timeline"));
        timelineTrack.insert(QStringLiteral("timescale"), 1000000);
        timelineTrack.insert(QStringLiteral("clock"), QStringLiteral("unix"));
        timelineTrack.insert(QStringLiteral("clockUnit"), QStringLiteral("microseconds"));
        timelineTrack.insert(QStringLiteral("referenceTrack"), catalog.track);

        QJsonObject updatePolicy;
        updatePolicy.insert(QStringLiteral("mode"), QStringLiteral("periodic"));
        updatePolicy.insert(QStringLiteral("intervalMs"), catalog.timelineIntervalMs);
        timelineTrack.insert(QStringLiteral("updatePolicy"), updatePolicy);

        tracks.append(timelineTrack);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("format"), QStringLiteral("msf"));
    root.insert(QStringLiteral("tracks"), tracks);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace moq2ts
