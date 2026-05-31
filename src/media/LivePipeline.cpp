#include "LivePipeline.h"

#include <algorithm>
#include <chrono>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include "LibavCaptureSource.h"
#include "MsftsMuxer.h"

namespace moq2ts {

namespace {

struct PcrSample {
    int pid = -1;
    std::uint64_t base90k = 0;
    int extension27m = 0;
};

std::uint64_t nowUnixUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::uint64_t nowSteadyUs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::optional<PcrSample> extractLastPcr(const QByteArray& payload, int packetSize, int pcrPid) {
    if (pcrPid < 0 || packetSize <= 0) {
        return std::nullopt;
    }

    const int syncOffset = packetSize == 192 ? 4 : 0;
    std::optional<PcrSample> sample;
    for (int offset = 0; offset + packetSize <= payload.size(); offset += packetSize) {
        const char* packet = payload.constData() + offset + syncOffset;
        if (static_cast<unsigned char>(packet[0]) != 0x47) {
            continue;
        }

        const int pid = ((static_cast<unsigned char>(packet[1]) & 0x1f) << 8) |
                        static_cast<unsigned char>(packet[2]);
        if (pid != pcrPid) {
            continue;
        }

        const int adaptationControl = (static_cast<unsigned char>(packet[3]) >> 4) & 0x03;
        if (adaptationControl != 2 && adaptationControl != 3) {
            continue;
        }

        const int adaptationLength = static_cast<unsigned char>(packet[4]);
        if (adaptationLength < 7 || 5 + adaptationLength > 188) {
            continue;
        }

        const int flags = static_cast<unsigned char>(packet[5]);
        if ((flags & 0x10) == 0) {
            continue;
        }

        const auto b0 = static_cast<std::uint64_t>(static_cast<unsigned char>(packet[6]));
        const auto b1 = static_cast<std::uint64_t>(static_cast<unsigned char>(packet[7]));
        const auto b2 = static_cast<std::uint64_t>(static_cast<unsigned char>(packet[8]));
        const auto b3 = static_cast<std::uint64_t>(static_cast<unsigned char>(packet[9]));
        const auto b4 = static_cast<std::uint64_t>(static_cast<unsigned char>(packet[10]));
        const auto b5 = static_cast<std::uint64_t>(static_cast<unsigned char>(packet[11]));
        PcrSample parsed;
        parsed.pid = pid;
        parsed.base90k = (b0 << 25) | (b1 << 17) | (b2 << 9) | (b3 << 1) | (b4 >> 7);
        parsed.extension27m = static_cast<int>(((b4 & 0x01) << 8) | b5);
        sample = parsed;
    }
    return sample;
}

QByteArray timelinePayload(const QString& mediaTrackName,
                           std::uint64_t mediaGroupId,
                           std::uint64_t mediaObjectId,
                           std::uint64_t mediaTimeUs,
                           std::uint64_t wallClockUnixUs,
                           const std::optional<PcrSample>& pcr) {
    QJsonObject clock;
    clock.insert(QStringLiteral("kind"), QStringLiteral("unix"));
    clock.insert(QStringLiteral("unixTimeUs"), QString::number(wallClockUnixUs));

    QJsonObject sender;
    sender.insert(QStringLiteral("monotonicTimeUs"), QString::number(nowSteadyUs()));
    sender.insert(QStringLiteral("timebase"), QStringLiteral("steady"));

    QJsonObject reference;
    reference.insert(QStringLiteral("track"), mediaTrackName);
    reference.insert(QStringLiteral("groupId"), QString::number(mediaGroupId));
    reference.insert(QStringLiteral("objectId"), QString::number(mediaObjectId));
    reference.insert(QStringLiteral("mediaTimeUs"), QString::number(mediaTimeUs));
    if (pcr.has_value()) {
        reference.insert(QStringLiteral("pcrPid"), pcr->pid);
        QJsonObject pcrObject;
        pcrObject.insert(QStringLiteral("pid"), pcr->pid);
        pcrObject.insert(QStringLiteral("base90k"), QString::number(pcr->base90k));
        pcrObject.insert(QStringLiteral("extension27m"), pcr->extension27m);
        reference.insert(QStringLiteral("pcr"), pcrObject);
    }

    QJsonObject mapping;
    mapping.insert(QStringLiteral("mediaTimeUs"), QString::number(mediaTimeUs));
    mapping.insert(QStringLiteral("wallClockUnixUs"), QString::number(wallClockUnixUs));
    mapping.insert(QStringLiteral("rateNumerator"), 1);
    mapping.insert(QStringLiteral("rateDenominator"), 1);

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("type"), QStringLiteral("timeline"));
    root.insert(QStringLiteral("clock"), clock);
    root.insert(QStringLiteral("sender"), sender);
    root.insert(QStringLiteral("reference"), reference);
    root.insert(QStringLiteral("mapping"), mapping);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
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
            .timestampMode = QStringLiteral("none"),
            .initData = capture.initData(),
            .timelineTrack = timelineTrackName,
            .timelineIntervalMs = 1000,
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
                timeline.payload = timelinePayload(trackName,
                                                   published.groupId,
                                                   published.objectId,
                                                   published.mediaTimeUs,
                                                   nowUnixUs(),
                                                   extractLastPcr(published.payload, capture.packetSize(), capture.pcrPid()));
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
        .timestampMode = packetizer.packetSize() == 192 ? QStringLiteral("m2ts") : QStringLiteral("none"),
        .initData = packetizer.initData(),
        .timelineTrack = timelineTrackName,
        .timelineIntervalMs = 1000,
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
            timeline.payload = timelinePayload(trackName,
                                               published.groupId,
                                               published.objectId,
                                               published.mediaTimeUs,
                                               nowUnixUs(),
                                               extractLastPcr(published.payload, packetizer.packetSize(), packetizer.pcrPid()));
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
