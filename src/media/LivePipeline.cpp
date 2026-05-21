#include "LivePipeline.h"

#include <algorithm>
#include <QDebug>
#include <QThread>

#include "LibavCaptureSource.h"
#include "MsftsMuxer.h"

namespace moq2ts {

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
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    emit status(QStringLiteral("Streaming pipeline stopped."));
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
        QString captureError;
        if (!capture.open(&captureError)) {
            emit error(captureError);
            m_running.store(false, std::memory_order_release);
            return;
        }

        const int packetsPerObject = std::max(1, m_config.targetSegmentBytes / capture.packetSize());
        const QString trackName = m_config.streamName.isEmpty() ? QStringLiteral("m2ts") : m_config.streamName;
        const QByteArray catalog = MsftsMuxer::catalogJson({
            .track = trackName,
            .packetSize = capture.packetSize(),
            .packetsPerObject = packetsPerObject,
            .programNumber = capture.programNumber(),
            .pmtPid = capture.pmtPid(),
            .pcrPid = capture.pcrPid(),
            .timestampMode = QStringLiteral("none"),
            .initData = capture.initData(),
        });

        int64_t objects = 0;
        int64_t bytes = 0;
        auto nextObject = [this, &capture, packetsPerObject, trackName, &objects, &bytes]() -> std::optional<PublishedObject> {
            if (!m_running.load(std::memory_order_acquire)) {
                return std::nullopt;
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
            ++objects;
            bytes += object.payload.size();
            emit stats(objects, bytes);
            return published;
        };

        if (!m_publisher->publishLiveObjects(m_config, trackName, catalog, std::move(nextObject), m_running)) {
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
    const QString trackName = m_config.streamName.isEmpty() ? QStringLiteral("m2ts") : m_config.streamName;
    const QByteArray catalog = MsftsMuxer::catalogJson({
        .track = trackName,
        .packetSize = packetizer.packetSize(),
        .packetsPerObject = packetsPerObject,
        .programNumber = packetizer.programNumber(),
        .pmtPid = packetizer.pmtPid(),
        .pcrPid = packetizer.pcrPid(),
        .timestampMode = packetizer.packetSize() == 192 ? QStringLiteral("m2ts") : QStringLiteral("none"),
        .initData = packetizer.initData(),
    });

    int64_t objects = 0;
    int64_t bytes = 0;
    auto nextObject = [this, &packetizer, packetsPerObject, trackName, &objects, &bytes]() -> std::optional<PublishedObject> {
        if (!m_running.load(std::memory_order_acquire)) {
            return std::nullopt;
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
        ++objects;
        bytes += object.payload.size();
        emit stats(objects, bytes);
        return published;
    };

    if (!m_publisher->publishLiveObjects(m_config, trackName, catalog, std::move(nextObject), m_running)) {
        if (m_running.load(std::memory_order_acquire)) {
            emit error(QStringLiteral("Failed to publish MSFTS live object stream."));
        }
    }

    m_running.store(false, std::memory_order_release);
    emit status(QStringLiteral("Pipeline exiting."));
}

} // namespace moq2ts
