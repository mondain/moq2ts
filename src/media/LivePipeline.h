#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <QImage>
#include <QObject>

#include "../app/PublishConfig.h"
#include "../publish/MoqxrPublisher.h"
#include "M2tsPacketizer.h"

namespace moq2ts {

class LivePipeline final : public QObject {
    Q_OBJECT
public:
    explicit LivePipeline(QObject* parent = nullptr);
    ~LivePipeline() override;

    void start(const PublishConfig& cfg, MoqxrPublisher* publisher);
    void stop();
    bool running() const;

signals:
    void status(const QString& message);
    void error(const QString& message);
    void stats(int64_t packets, int64_t bytes);
    void previewVideoFrame(const QImage& image);
    void previewAudioLevels(double left, double right);

private:
    void runLoop();

    PublishConfig m_config;
    MoqxrPublisher* m_publisher = nullptr;

    std::atomic<bool> m_running {false};
    std::thread m_workerThread;
};

} // namespace moq2ts
