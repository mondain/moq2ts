#include "LivePipeline.h"

#include <algorithm>
#include <chrono>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include "LibavCaptureSource.h"
#include "MsftsMuxer.h"

namespace moq2ts {

namespace {

std::uint64_t nowUnixUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// Builds an MSF media timeline payload (draft-ietf-moq-msf-00 Section 7.1): a
// JSON array of records, each a three-item array
//   [ mediaPresentationTimeMs, [groupId, objectId], wallclockMs ]
// where wallclock is milliseconds since the Unix epoch (0 when unknown). A
// single timeline object carries one record here.
QByteArray timelinePayload(std::uint64_t mediaGroupId,
                           std::uint64_t mediaObjectId,
                           std::uint64_t mediaTimeUs,
                           std::uint64_t wallClockUnixUs) {
    QJsonArray location;
    location.append(static_cast<qint64>(mediaGroupId));
    location.append(static_cast<qint64>(mediaObjectId));

    QJsonArray record;
    record.append(static_cast<qint64>((mediaTimeUs + 500ULL) / 1000ULL));
    record.append(location);
    record.append(static_cast<qint64>((wallClockUnixUs + 500ULL) / 1000ULL));

    QJsonArray records;
    records.append(record);
    return QJsonDocument(records).toJson(QJsonDocument::Compact);
}

} // namespace

LivePipeline::LivePipeline(QObject* parent)
    : QObject(parent) {}

LivePipeline::~LivePipeline() {
    stop();
}

bool LivePipeline::running() const {
    return m_running.load(std::memory_order_acquire);
}

void LivePipeline::start(const PublishConfig& cfg, MoqxrPublisher* publisher) {
    if (m_running.load(std::memory_order_acquire)) {
        emit status(QStringLiteral("Pipeline already running."));
        return;
    }

    if (!publisher) {
        emit error(QStringLiteral("No publisher instance provided."));
        return;
    }

    m_config = cfg;
    m_publisher = publisher;
    m_running.store(true, std::memory_order_release);

    const bool hasCaptureSource = !cfg.cameraDeviceId.isEmpty() || !cfg.microphoneDeviceId.isEmpty();
    if (cfg.videoSource.isEmpty() && cfg.audioSource.isEmpty() && !hasCaptureSource) {
        emit error(QStringLiteral("An M2TS source or capture device is required."));
        m_running.store(false, std::memory_order_release);
        return;
    }
    if (!cfg.videoSource.isEmpty() && !cfg.audioSource.isEmpty() && cfg.videoSource != cfg.audioSource) {
        emit error(QStringLiteral("draft-gregoire-moq-msfts carries a single ordered TS/M2TS packet stream per track. Use one multiplexed M2TS source for this scaffold."));
        m_running.store(false, std::memory_order_release);
        return;
    }

    m_workerThread = std::thread([this]() { runLoop(); });
    emit status(QStringLiteral("MSFTS M2TS packet pipeline started."));
}

void LivePipeline::stop() {
    const bool wasRunning = m_running.load(std::memory_order_acquire);
    requestStop();
    waitForStopped();

    if (wasRunning) {
        emit status(QStringLiteral("Streaming pipeline stopped."));
    }
}

void LivePipeline::requestStop() {
    m_running.store(false, std::memory_order_release);
}

void LivePipeline::waitForStopped() {
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void LivePipeline::runLoop() {
    if (!m_publisher) {
        emit error(QStringLiteral("Pipeline is missing publisher."));
        m_running.store(false, std::memory_order_release);
        return;
    }

    if ((m_config.videoSource.isEmpty() && m_config.audioSource.isEmpty()) &&
        (!m_config.cameraDeviceId.isEmpty() || !m_config.microphoneDeviceId.isEmpty())) {
        LibavCaptureSource capture(m_config);
        capture.setPreviewCallbacks(
            [this](const QImage& image) {
                emit previewVideoFrame(image);
            },
            [this](double left, double right) {
                emit previewAudioLevels(left, right);
            });
        QString captureError;
        if (!capture.open(&captureError)) {
            emit error(captureError);
            m_running.store(false, std::memory_order_release);
            return;
        }

        const int packetsPerObject = std::max(1, m_config.targetSegmentBytes / capture.packetSize());
        const QString trackName = QStringLiteral("m2ts");
        const QString timelineTrackName = trackName + QStringLiteral(".timeline");
        const QByteArray catalog = MsftsMuxer::catalogJson({
            .track = trackName,
            .packetSize = capture.packetSize(),
            .packetsPerObject = packetsPerObject,
            .programNumber = capture.programNumber(),
            .pmtPid = capture.pmtPid(),
            .pcrPid = capture.pcrPid(),
            .initData = capture.initData(),
            .timelineTrack = timelineTrackName,
            .bitrateBps = static_cast<qint64>(m_config.videoTargetBitrateKbps) * 1000,
            .generatedAtMs = QDateTime::currentMSecsSinceEpoch(),
        });

        int64_t objects = 0;
        int64_t bytes = 0;
        std::uint64_t timelineObjectId = 0;
        std::optional<PublishedObject> pendingTimeline;
        const int timelineEveryObjects = std::max(1, 1000 / std::max(1, m_config.fragmentDurationMs));
        auto nextObject = [this, &capture, packetsPerObject, trackName, timelineTrackName, timelineEveryObjects, &objects, &bytes, &timelineObjectId, &pendingTimeline]() -> std::optional<PublishedObject> {
            if (!m_running.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            if (pendingTimeline.has_value()) {
                auto timeline = std::move(pendingTimeline);
                pendingTimeline.reset();
                return timeline;
            }

            M2tsObject object;
            QString readError;
            if (!capture.readObject(packetsPerObject, &object, m_running, &readError)) {
                if (!readError.isEmpty()) {
                    emit error(readError);
                }
                return std::nullopt;
            }

            PublishedObject published;
            published.trackName = trackName;
            published.payload = std::move(object.payload);
            published.groupId = object.groupId;
            published.objectId = object.objectId;
            published.mediaTimeUs = static_cast<std::uint64_t>(objects) * static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
            published.mediaDurationUs = static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
            if ((objects % timelineEveryObjects) == 0) {
                PublishedObject timeline;
                timeline.trackName = timelineTrackName;
                timeline.payload = timelinePayload(published.groupId,
                                                   published.objectId,
                                                   published.mediaTimeUs,
                                                   nowUnixUs());
                timeline.groupId = published.groupId;
                timeline.objectId = timelineObjectId++;
                timeline.mediaTimeUs = published.mediaTimeUs;
                timeline.mediaDurationUs = 0;
                pendingTimeline = std::move(timeline);
            }
            ++objects;
            bytes += published.payload.size();
            emit stats(objects, bytes);
            return published;
        };

        if (!m_publisher->publishLiveObjects(m_config, trackName, QStringList{timelineTrackName}, catalog, std::move(nextObject), m_running)) {
            if (m_running.load(std::memory_order_acquire)) {
                emit error(QStringLiteral("Failed to publish captured MSFTS live object stream."));
            }
        }

        m_running.store(false, std::memory_order_release);
        emit status(QStringLiteral("Pipeline exiting."));
        return;
    }

    const QString sourcePath = !m_config.videoSource.isEmpty() ? m_config.videoSource : m_config.audioSource;
    M2tsPacketizer packetizer(sourcePath);
    QString packetizerError;
    if (!packetizer.open(m_config.programNumber, &packetizerError)) {
        emit error(packetizerError);
        m_running.store(false, std::memory_order_release);
        return;
    }

    const int packetsPerObject = std::max(1, m_config.targetSegmentBytes / packetizer.packetSize());
    const QString trackName = QStringLiteral("m2ts");
    const QString timelineTrackName = trackName + QStringLiteral(".timeline");
    const QByteArray catalog = MsftsMuxer::catalogJson({
        .track = trackName,
        .packetSize = packetizer.packetSize(),
        .packetsPerObject = packetsPerObject,
        .programNumber = packetizer.programNumber(),
        .pmtPid = packetizer.pmtPid(),
        .pcrPid = packetizer.pcrPid(),
        // For 192-octet source packets the timestamp prefix is carried without
        // specified semantics ("opaque", MSFTS 6.9); omitted for 188.
        .timestampMode = packetizer.packetSize() == 192 ? QStringLiteral("opaque") : QString(),
        .initData = packetizer.initData(),
        .timelineTrack = timelineTrackName,
        .bitrateBps = static_cast<qint64>(m_config.videoTargetBitrateKbps) * 1000,
        .generatedAtMs = QDateTime::currentMSecsSinceEpoch(),
    });

    int64_t objects = 0;
    int64_t bytes = 0;
    std::uint64_t timelineObjectId = 0;
    std::optional<PublishedObject> pendingTimeline;
    const int timelineEveryObjects = std::max(1, 1000 / std::max(1, m_config.fragmentDurationMs));
    auto nextObject = [this, &packetizer, packetsPerObject, trackName, timelineTrackName, timelineEveryObjects, &objects, &bytes, &timelineObjectId, &pendingTimeline]() -> std::optional<PublishedObject> {
        if (!m_running.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        if (pendingTimeline.has_value()) {
            auto timeline = std::move(pendingTimeline);
            pendingTimeline.reset();
            return timeline;
        }

        M2tsObject object;
        QString readError;
        if (!packetizer.readObject(packetsPerObject, &object, &readError)) {
            if (!readError.isEmpty()) {
                emit error(readError);
            }
            return std::nullopt;
        }

        PublishedObject published;
        published.trackName = trackName;
        published.payload = std::move(object.payload);
        published.groupId = object.groupId;
        published.objectId = object.objectId;
        published.mediaTimeUs = static_cast<std::uint64_t>(objects) * static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
        published.mediaDurationUs = static_cast<std::uint64_t>(m_config.fragmentDurationMs) * 1000ULL;
        if ((objects % timelineEveryObjects) == 0) {
            PublishedObject timeline;
            timeline.trackName = timelineTrackName;
            timeline.payload = timelinePayload(published.groupId,
                                               published.objectId,
                                               published.mediaTimeUs,
                                               nowUnixUs());
            timeline.groupId = published.groupId;
            timeline.objectId = timelineObjectId++;
            timeline.mediaTimeUs = published.mediaTimeUs;
            timeline.mediaDurationUs = 0;
            pendingTimeline = std::move(timeline);
        }
        ++objects;
        bytes += published.payload.size();
        emit stats(objects, bytes);
        return published;
    };

    if (!m_publisher->publishLiveObjects(m_config, trackName, QStringList{timelineTrackName}, catalog, std::move(nextObject), m_running)) {
        if (m_running.load(std::memory_order_acquire)) {
            emit error(QStringLiteral("Failed to publish MSFTS live object stream."));
        }
    }

    m_running.store(false, std::memory_order_release);
    emit status(QStringLiteral("Pipeline exiting."));
}

} // namespace moq2ts
