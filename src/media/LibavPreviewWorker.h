#pragma once

#include <QObject>
#include <QImage>

#include <atomic>

#include "../app/PublishConfig.h"

namespace moq2ts {

class LibavPreviewWorker final : public QObject {
    Q_OBJECT
public:
    explicit LibavPreviewWorker(QObject* parent = nullptr);

public slots:
    void start(const moq2ts::PublishConfig& config);
    void stop();

signals:
    void videoFrameReady(const QImage& image);
    void audioLevelsChanged(double left, double right);
    void status(const QString& message);
    void error(const QString& message);
    void finished();

public:
    // UI decrements this in its frame handler; the worker skips emitting new
    // frames while > 0 so a slow UI thread cannot accumulate a queue backlog.
    std::atomic<int> m_pendingFrames{0};

private:
    std::atomic<bool> m_running = false;
};

} // namespace moq2ts
