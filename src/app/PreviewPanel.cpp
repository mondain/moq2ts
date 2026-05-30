#include "PreviewPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "../media/LibavPreviewWorker.h"

namespace moq2ts {

PreviewPanel::PreviewPanel(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    m_videoLabel = new QLabel();
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setMinimumSize(480, 270);
    m_videoLabel->setStyleSheet(QStringLiteral("background:#111827;color:#f9fafb;border:1px solid #374151;"));
    m_videoLabel->setScaledContents(false);
    showNoSignal(QStringLiteral("No preview running"));

    m_leftMeter = new QProgressBar();
    m_rightMeter = new QProgressBar();
    for (QProgressBar* meter : {m_leftMeter, m_rightMeter}) {
        meter->setRange(0, 100);
        meter->setTextVisible(false);
        meter->setFixedHeight(16);
    }

    auto* meterLayout = new QVBoxLayout();
    auto* leftRow = new QHBoxLayout();
    leftRow->addWidget(new QLabel(QStringLiteral("L")));
    leftRow->addWidget(m_leftMeter, 1);
    auto* rightRow = new QHBoxLayout();
    rightRow->addWidget(new QLabel(QStringLiteral("R")));
    rightRow->addWidget(m_rightMeter, 1);
    meterLayout->addLayout(leftRow);
    meterLayout->addLayout(rightRow);

    m_statusLabel = new QLabel(QStringLiteral("Preview idle"));
    m_statusLabel->setWordWrap(true);

    m_startButton = new QPushButton(QStringLiteral("Start preview"));
    m_stopButton = new QPushButton(QStringLiteral("Stop preview"));
    m_stopButton->setEnabled(false);
    auto* buttonRow = new QHBoxLayout();
    buttonRow->addWidget(m_startButton);
    buttonRow->addWidget(m_stopButton);
    buttonRow->addStretch(1);

    root->addWidget(m_videoLabel, 1);
    root->addLayout(meterLayout);
    root->addWidget(m_statusLabel);
    root->addLayout(buttonRow);

    QObject::connect(m_startButton, &QPushButton::clicked, this, &PreviewPanel::startPreview);
    QObject::connect(m_stopButton, &QPushButton::clicked, this, &PreviewPanel::stopPreview);
}

PreviewPanel::~PreviewPanel() {
    stopPreview();
}

void PreviewPanel::setConfig(const PublishConfig& config) {
    m_config = config;
}

void PreviewPanel::setControlsEnabled(bool enabled) {
    m_startButton->setEnabled(enabled && !m_previewRunning);
}

void PreviewPanel::startPreview() {
    if (m_previewRunning) {
        return;
    }
    if (m_config.cameraDeviceId.isEmpty() && m_config.microphoneDeviceId.isEmpty()) {
        handlePreviewError(QStringLiteral("Select a camera or microphone before starting preview."));
        return;
    }

    m_thread = new QThread(this);
    m_worker = new LibavPreviewWorker();
    m_worker->moveToThread(m_thread);

    QObject::connect(this, &PreviewPanel::stopWorker, m_worker, &LibavPreviewWorker::stop, Qt::DirectConnection);
    QObject::connect(m_worker, &LibavPreviewWorker::videoFrameReady, this, &PreviewPanel::handleVideoFrame);
    QObject::connect(m_worker, &LibavPreviewWorker::audioLevelsChanged, this, &PreviewPanel::handleAudioLevels);
    QObject::connect(m_worker, &LibavPreviewWorker::status, this, &PreviewPanel::handlePreviewStatus);
    QObject::connect(m_worker, &LibavPreviewWorker::error, this, &PreviewPanel::handlePreviewError);
    QObject::connect(m_worker, &LibavPreviewWorker::finished, this, &PreviewPanel::handlePreviewFinished);
    QObject::connect(m_worker, &LibavPreviewWorker::finished, m_thread, &QThread::quit);
    QObject::connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    QObject::connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);

    m_previewRunning = true;
    m_startButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    handlePreviewStatus(QStringLiteral("Starting preview..."));

    m_thread->start();
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, config = m_config]() {
        worker->start(config);
    }, Qt::QueuedConnection);
}

void PreviewPanel::stopPreview() {
    if (!m_previewRunning) {
        return;
    }
    emit stopWorker();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
    handlePreviewFinished();
}

void PreviewPanel::handleVideoFrame(const QImage& image) {
    if (image.isNull()) {
        return;
    }
    const QPixmap pixmap = QPixmap::fromImage(image).scaled(m_videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_videoLabel->setPixmap(pixmap);
    m_hasVideoFrame = true;
}

void PreviewPanel::handleAudioLevels(double left, double right) {
    updateMeter(m_leftMeter, left);
    updateMeter(m_rightMeter, right);
}

void PreviewPanel::handlePreviewStatus(const QString& message) {
    m_statusLabel->setText(message);
}

void PreviewPanel::handlePreviewError(const QString& message) {
    showNoSignal(QStringLiteral("Preview unavailable"));
    m_statusLabel->setText(message);
}

void PreviewPanel::handlePreviewFinished() {
    m_previewRunning = false;
    m_worker = nullptr;
    m_thread = nullptr;
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    updateMeter(m_leftMeter, 0.0);
    updateMeter(m_rightMeter, 0.0);
    if (!m_hasVideoFrame) {
        showNoSignal(QStringLiteral("No preview running"));
    }
}

void PreviewPanel::updateMeter(QProgressBar* meter, double level) {
    if (level <= 0.0) {
        meter->setValue(0);
        return;
    }
    constexpr double minDb = -60.0;
    const double db = 20.0 * std::log10(std::clamp(level, 0.000001, 1.0));
    const double normalized = (std::clamp(db, minDb, 0.0) - minDb) / -minDb;
    const int value = std::clamp(static_cast<int>(normalized * 100.0), 0, 100);
    meter->setValue(value);
}

void PreviewPanel::showNoSignal(const QString& message) {
    m_videoLabel->setPixmap(QPixmap());
    m_videoLabel->setText(message);
    m_hasVideoFrame = false;
}

} // namespace moq2ts
