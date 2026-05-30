#include "MoqxrPublisher.h"

#include <QDebug>
#include <QMutexLocker>
#include <QThread>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef MOQ2TS_HAS_MOQXR
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/publisher_api.h"
#endif

namespace {

bool useMockTransport(const moq2ts::PublishConfig& cfg) {
    return cfg.moqEndpoint.trimmed().startsWith(QStringLiteral("mock://"), Qt::CaseInsensitive);
}

#ifdef MOQ2TS_HAS_MOQXR
openmoq::publisher::transport::EndpointConfig parseEndpoint(const QString& rawEndpoint) {
    using namespace openmoq::publisher::transport;

    EndpointConfig endpoint;
    std::string authority = rawEndpoint.trimmed().toStdString();
    const auto consumeScheme = [&](const char* prefix) {
        const std::string value(prefix);
        if (authority.rfind(value, 0) == 0) {
            authority = authority.substr(value.size());
            return true;
        }
        return false;
    };

    const bool hadMoqt = consumeScheme("moqt://");
    const bool hadHttps = consumeScheme("https://");
    const bool hadHttp = consumeScheme("http://");
    if (hadMoqt || hadHttps || hadHttp) {
        const std::size_t slash = authority.find('/');
        if (slash != std::string::npos) {
            endpoint.path = authority.substr(slash);
            endpoint.path_explicit = true;
            authority = authority.substr(0, slash);
        } else if (hadHttps || hadHttp) {
            endpoint.path.clear();
            endpoint.path.push_back('/');
            endpoint.path_explicit = true;
        }
        endpoint.transport = (hadHttps || hadHttp) ? TransportKind::kWebTransport : TransportKind::kRawQuic;
    }

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size()) {
        throw std::runtime_error("endpoint must be host:port, moqt://host:port/path, or https://host:port/path");
    }
    endpoint.host = authority.substr(0, colon);
    const int port = std::stoi(authority.substr(colon + 1));
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("endpoint port must be between 1 and 65535");
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    if (endpoint.transport == TransportKind::kWebTransport && !endpoint.path_explicit) {
        endpoint.path.clear();
        endpoint.path.push_back('/');
        endpoint.path.append("moq");
        endpoint.path_explicit = true;
    }
    return endpoint;
}

std::vector<std::uint8_t> toVector(const QByteArray& bytes) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    return std::vector<std::uint8_t>(begin, begin + bytes.size());
}

openmoq::publisher::DraftVersion defaultDraftVersion() {
    return openmoq::publisher::DraftVersion::kDraft16;
}

#endif

} // namespace

namespace moq2ts {

MoqxrPublisher::MoqxrPublisher(QObject* parent)
    : QObject(parent) {}

MoqxrPublisher::~MoqxrPublisher() {
    stop();
}

bool MoqxrPublisher::connect(const PublishConfig& cfg) {
    QMutexLocker locker(&m_mutex);
    m_endpoint = cfg.moqEndpoint;
    m_namespace = cfg.namespaceName;
    m_streamName = cfg.streamName;

#ifdef MOQ2TS_HAS_MOQXR
    if (useMockTransport(cfg)) {
        return connectMock(cfg);
    }

    m_connected = true;
    emit connectionStateChanged(true, QString("moqxr publisher configured for %1; namespace=%2 stream=%3").arg(m_endpoint, m_namespace, m_streamName));
    return true;
#else
    if (!useMockTransport(cfg)) {
        emit publishError("Mock publisher builds only accept mock:// endpoints. Set MOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF and rebuild to publish to a real relay.");
        return false;
    }
    return connectMock(cfg);
#endif
}

bool MoqxrPublisher::connectMock(const PublishConfig& cfg) {
    if (cfg.moqEndpoint.isEmpty() || cfg.namespaceName.isEmpty() || cfg.streamName.isEmpty()) {
        emit publishError("Invalid MOQ endpoint/namespace/stream configuration.");
        return false;
    }

    m_connected = true;
    emit connectionStateChanged(true, QString("Mock publisher connected to %1; namespace=%2 stream=%3").arg(m_endpoint, m_namespace, m_streamName));
    return true;
}

bool MoqxrPublisher::publishLiveObjects(const PublishConfig& cfg,
                                        const QString& mediaTrackName,
                                        const QStringList& sideTrackNames,
                                        const QByteArray& catalog,
                                        std::function<std::optional<PublishedObject>()> nextObject,
                                        std::atomic<bool>& running) {
#ifdef MOQ2TS_HAS_MOQXR
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected) {
            emit publishError("Cannot publish: no active moqxr session.");
            return false;
        }
    }

    if (useMockTransport(cfg)) {
        return publishLiveObjectsMock(catalog, std::move(nextObject), running, cfg.fragmentDurationMs);
    }

    try {
        openmoq::publisher::PublisherConfig publisherConfig;
        publisherConfig.draft_version = defaultDraftVersion();
        publisherConfig.track_namespace = cfg.namespaceName.toStdString();
        publisherConfig.forward = true;
        publisherConfig.publish_catalog = false;
        publisherConfig.paced = false;
        publisherConfig.loop = false;
        publisherConfig.subscriber_timeout = std::chrono::seconds(30);

        openmoq::publisher::LiveObjectSource source;
        source.tracks.push_back({.track_name = "catalog"});
        source.tracks.push_back({.track_name = mediaTrackName.toStdString()});
        for (const QString& trackName : sideTrackNames) {
            source.tracks.push_back({.track_name = trackName.toStdString()});
        }

        bool catalogSent = false;
        source.next_object = [catalogSent, catalog, nextObject = std::move(nextObject)]() mutable -> std::optional<openmoq::publisher::LiveObject> {
            if (!catalogSent) {
                catalogSent = true;
                return openmoq::publisher::LiveObject{
                    .track_name = "catalog",
                    .group_id = 0,
                    .subgroup_id = 0,
                    .object_id = 0,
                    .media_time_us = 0,
                    .media_duration_us = 0,
                    .payload = toVector(catalog),
                    .subgroup_contains_group_largest = true,
                    .final_in_subgroup = true,
                };
            }

            std::optional<PublishedObject> next = nextObject();
            if (!next.has_value()) {
                return std::nullopt;
            }
            return openmoq::publisher::LiveObject{
                .track_name = next->trackName.toStdString(),
                .group_id = static_cast<std::size_t>(next->groupId),
                .subgroup_id = next->subgroupId,
                .object_id = static_cast<std::size_t>(next->objectId),
                .media_time_us = next->mediaTimeUs,
                .media_duration_us = next->mediaDurationUs,
                .payload = toVector(next->payload),
                .subgroup_contains_group_largest = true,
                .final_in_subgroup = true,
            };
        };

        openmoq::publisher::Publisher publisher(publisherConfig);
        openmoq::publisher::transport::TlsConfig tls;
        tls.insecure_skip_verify = true;
        const auto status = publisher.publish_live_objects(source, parseEndpoint(cfg.moqEndpoint), tls, false);
        if (!status.ok && running.load(std::memory_order_acquire)) {
            emit publishError(QString::fromStdString(status.message));
            return false;
        }
        const auto disconnectStatus = publisher.disconnect(0);
        if (!disconnectStatus.ok) {
            emit publishError(QString::fromStdString(disconnectStatus.message));
            return false;
        }

        const auto stats = publisher.stats();
        emit framePublished(mediaTrackName, static_cast<int64_t>(stats.bytes_published), static_cast<int64_t>(stats.objects_published));
        return status.ok;
    } catch (const std::exception& error) {
        emit publishError(QString::fromStdString(error.what()));
        return false;
    }
#else
    Q_UNUSED(cfg);
    Q_UNUSED(mediaTrackName);
    Q_UNUSED(sideTrackNames);
    return publishLiveObjectsMock(catalog, std::move(nextObject), running, cfg.fragmentDurationMs);
#endif
}

bool MoqxrPublisher::publishLiveObjectsMock(const QByteArray& catalog,
                                            std::function<std::optional<PublishedObject>()> nextObject,
                                            std::atomic<bool>& running,
                                            int pacingMs) {
    {
        QMutexLocker locker(&m_mutex);
        if (!m_connected) {
            emit publishError("Cannot publish: mock publisher not connected.");
            return false;
        }
    }

    qDebug() << "MOCK MSFTS catalog bytes=" << catalog.size();
    emit framePublished(QStringLiteral("catalog"), catalog.size(), 0);

    while (running.load(std::memory_order_acquire)) {
        std::optional<PublishedObject> object = nextObject();
        if (!object.has_value()) {
            return true;
        }
        qDebug() << "MOCK MSFTS publish" << object->trackName << "group=" << object->groupId
                 << "object=" << object->objectId << "bytes=" << object->payload.size();
        emit framePublished(object->trackName, object->payload.size(), static_cast<int64_t>(object->objectId));
        if (pacingMs > 0) {
            QThread::msleep(static_cast<unsigned long>(pacingMs));
        }
    }
    return true;
}


void MoqxrPublisher::stop() {
    QMutexLocker locker(&m_mutex);
    if (!m_connected) {
        return;
    }
    m_connected = false;
    emit connectionStateChanged(false, "Publisher stopped");
}

bool MoqxrPublisher::connected() const {
    QMutexLocker locker(&m_mutex);
    return m_connected;
}

} // namespace moq2ts
