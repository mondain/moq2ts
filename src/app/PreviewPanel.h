#pragma once

#include <QImage>
#include <QWidget>

#include "PublishConfig.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QThread;

namespace moq2ts {

class LibavPreviewWorker;

class PreviewPanel final : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPanel(QWidget* parent = nullptr);
    ~PreviewPanel() override;

    void setConfig(const PublishConfig& config);
    void setControlsEnabled(bool enabled);
    void stopPreview();

public slots:
    void handleVideoFrame(const QImage& image);
    void handleAudioLevels(double left, double right);

signals:
    void stopWorker();

private slots:
    void startPreview();
    void handlePreviewStatus(const QString& message);
    void handlePreviewError(const QString& message);
    void handlePreviewFinished();

private:
    void updateMeter(QProgressBar* meter, double level);
    void showNoSignal(const QString& message);

    PublishConfig m_config;
    QThread* m_thread = nullptr;
    LibavPreviewWorker* m_worker = nullptr;
    QLabel* m_videoLabel = nullptr;
    QProgressBar* m_leftMeter = nullptr;
    QProgressBar* m_rightMeter = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    bool m_previewRunning = false;
    bool m_hasVideoFrame = false;
};

} // namespace moq2ts
