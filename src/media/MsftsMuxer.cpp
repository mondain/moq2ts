#include "MsftsMuxer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace moq2ts {

QByteArray MsftsMuxer::catalogJson(const MsftsCatalog& catalog) {
    QJsonObject mediaTrack;
    mediaTrack.insert(QStringLiteral("name"), catalog.track);
    mediaTrack.insert(QStringLiteral("packaging"), QStringLiteral("m2ts"));
    // MSF common track fields (draft-ietf-moq-msf-00).
    mediaTrack.insert(QStringLiteral("isLive"), catalog.isLive);
    mediaTrack.insert(QStringLiteral("role"), catalog.role);
    mediaTrack.insert(QStringLiteral("mimeType"), catalog.mimeType);
    if (catalog.isLive) {
        // targetLatency MUST NOT be present when isLive is false (MSF 5.1.16).
        mediaTrack.insert(QStringLiteral("targetLatency"), catalog.targetLatencyMs);
    }
    if (catalog.bitrateBps > 0) {
        mediaTrack.insert(QStringLiteral("bitrate"), catalog.bitrateBps);
    }
    // MSFTS m2ts-specific fields (draft-gregoire-moq-msfts-00 Section 6).
    mediaTrack.insert(QStringLiteral("m2tsPacketSize"), catalog.packetSize);
    mediaTrack.insert(QStringLiteral("m2tsPacketsPerObject"), catalog.packetsPerObject);
    mediaTrack.insert(QStringLiteral("m2tsProgramNumber"), catalog.programNumber);
    if (catalog.pmtPid >= 0) {
        mediaTrack.insert(QStringLiteral("m2tsPmtPid"), catalog.pmtPid);
    }
    if (catalog.pcrPid >= 0) {
        mediaTrack.insert(QStringLiteral("m2tsPcrPid"), catalog.pcrPid);
    }
    // m2tsTimestampMode is only valid for 192-octet source packets (MSFTS 6.9);
    // it MUST NOT be present for 188.
    if (catalog.packetSize == 192 && !catalog.timestampMode.isEmpty()) {
        mediaTrack.insert(QStringLiteral("m2tsTimestampMode"), catalog.timestampMode);
    }
    if (!catalog.initData.isEmpty()) {
        mediaTrack.insert(QStringLiteral("initData"), QString::fromLatin1(catalog.initData.toBase64()));
    }

    QJsonArray tracks;
    tracks.append(mediaTrack);

    if (!catalog.timelineTrack.isEmpty()) {
        // MSF media timeline track (draft-ietf-moq-msf-00 Section 7.2): identified
        // by type "mediatimeline", a "depends" list of the track names it applies
        // to, and an application/json mime type.
        QJsonObject timelineTrack;
        timelineTrack.insert(QStringLiteral("name"), catalog.timelineTrack);
        timelineTrack.insert(QStringLiteral("type"), QStringLiteral("mediatimeline"));
        QJsonArray depends;
        depends.append(catalog.track);
        timelineTrack.insert(QStringLiteral("depends"), depends);
        timelineTrack.insert(QStringLiteral("mimeType"), QStringLiteral("application/json"));

        tracks.append(timelineTrack);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    if (catalog.isLive && catalog.generatedAtMs > 0) {
        // SHOULD NOT be included when isLive is false (MSF 5.1.6).
        root.insert(QStringLiteral("generatedAt"), catalog.generatedAtMs);
    }
    root.insert(QStringLiteral("tracks"), tracks);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace moq2ts
