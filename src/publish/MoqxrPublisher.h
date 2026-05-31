#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "../app/PublishConfig.h"

#ifdef MOQ2TS_HAS_MOQXR
namespace openmoq::publisher {
class Publisher;
}
#endif

namespace moq2ts {

struct PublishedObject {
    QString trackName;
    QByteArray payload;
    std::uint64_t groupId = 0;
    std::uint64_t subgroupId = 0;
    std::uint64_t objectId = 0;
    std::uint64_t mediaTimeUs = 0;
    std::uint64_t mediaDurationUs = 0;
};

class IMoqOutput {
public:
    virtual ~IMoqOutput() = default;
    virtual bool connect(const PublishConfig& cfg) = 0;
    virtual bool publishLiveObjects(const PublishConfig& cfg,
                                    const QString& mediaTrackName,
                                    const QStringList& sideTrackNames,
                                    const QByteArray& catalog,
                                    std::function<std::optional<PublishedObject>()> nextObject,
                                    std::atomic<bool>& running) = 0;
    virtual void stop() = 0;
    virtual bool connected() const = 0;
};

class MoqxrPublisher final : public QObject, public IMoqOutput {
    Q_OBJECT
public:
    explicit MoqxrPublisher(QObject* parent = nullptr);
    ~MoqxrPublisher() override;

    bool connect(const PublishConfig& cfg) override;
    bool publishLiveObjects(const PublishConfig& cfg,
                            const QString& mediaTrackName,
                            const QStringList& sideTrackNames,
                            const QByteArray& catalog,
                            std::function<std::optional<PublishedObject>()> nextObject,
                            std::atomic<bool>& running) override;
    void stop() override;
    bool connected() const override;

signals:
    void connectionStateChanged(bool connected, const QString& message);
    void framePublished(const QString& track, int64_t bytes, int64_t objects);
    void publishError(const QString& error);

private:
    bool connectMock(const PublishConfig& cfg);
    bool publishLiveObjectsMock(const QByteArray& catalog,
                                std::function<std::optional<PublishedObject>()> nextObject,
                                std::atomic<bool>& running,
                                int pacingMs);

    bool m_connected = false;
    QString m_namespace;
    QString m_endpoint;
    mutable QMutex m_mutex;
#ifdef MOQ2TS_HAS_MOQXR
    std::shared_ptr<openmoq::publisher::Publisher> m_activePublisher;
#endif
};

} // namespace moq2ts
