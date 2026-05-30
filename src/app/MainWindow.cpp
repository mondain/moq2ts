#include "MainWindow.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QTabWidget>

#include "../media/LibavCaptureSource.h"

#ifdef MOQ2TS_HAVE_QT_MULTIMEDIA
#if !defined(Q_OS_DARWIN)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioDevice>
#include <QCameraDevice>
#include <QMediaDevices>
#else
#include <QAudio>
#include <QAudioDeviceInfo>
#include <QCameraInfo>
#endif
#endif
#endif

namespace moq2ts {

MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle("MOQ M2TS Live Publisher");
    setMinimumSize(720, 560);

    auto* root = new QVBoxLayout(this);
    tabs = new QTabWidget();
    auto* configTab = new QWidget();
    auto* configLayout = new QVBoxLayout(configTab);
    auto* logsTab = new QWidget();
    auto* logsLayout = new QVBoxLayout(logsTab);
    previewPanel = new PreviewPanel();

    auto* form = new QFormLayout();

    endpointEdit = new QLineEdit("mock://local");
    namespaceEdit = new QLineEdit("live");
    streamNameEdit = new QLineEdit("sample-stream");

    videoSourceEdit = new QLineEdit();
    auto* videoBrowse = new QPushButton("Browse");
    QObject::connect(videoBrowse, &QPushButton::clicked, this, &MainWindow::selectVideoSource);
    auto* videoRow = new QWidget();
    auto* videoRowLayout = new QHBoxLayout(videoRow);
    videoRowLayout->setContentsMargins(0, 0, 0, 0);
    videoRowLayout->addWidget(videoSourceEdit);
    videoRowLayout->addWidget(videoBrowse);

    audioSourceEdit = new QLineEdit();
    auto* audioBrowse = new QPushButton("Browse");
    QObject::connect(audioBrowse, &QPushButton::clicked, this, &MainWindow::selectAudioSource);
    auto* audioRow = new QWidget();
    auto* audioRowLayout = new QHBoxLayout(audioRow);
    audioRowLayout->setContentsMargins(0, 0, 0, 0);
    audioRowLayout->addWidget(audioSourceEdit);
    audioRowLayout->addWidget(audioBrowse);

    cameraSourceCombo = new QComboBox();
    microphoneSourceCombo = new QComboBox();
    auto* refreshDevicesButton = new QPushButton("Refresh capture devices");
    QObject::connect(refreshDevicesButton, &QPushButton::clicked, this, &MainWindow::refreshCaptureDevices);

    widthSpin = new QSpinBox();
    widthSpin->setRange(160, 7680);
    widthSpin->setValue(1920);

    heightSpin = new QSpinBox();
    heightSpin->setRange(120, 4320);
    heightSpin->setValue(1080);

    fpsSpin = new QSpinBox();
    fpsSpin->setRange(1, 120);
    fpsSpin->setValue(30);

    videoBitrateSpin = new QSpinBox();
    videoBitrateSpin->setRange(200, 20000);
    videoBitrateSpin->setValue(2500);

    sampleRateSpin = new QSpinBox();
    sampleRateSpin->setRange(8000, 192000);
    sampleRateSpin->setValue(48000);

    channelSpin = new QSpinBox();
    channelSpin->setRange(1, 8);
    channelSpin->setValue(2);

    audioBitrateSpin = new QSpinBox();
    audioBitrateSpin->setRange(32, 512);
    audioBitrateSpin->setValue(160);

    fragmentMsSpin = new QSpinBox();
    fragmentMsSpin->setRange(50, 4000);
    fragmentMsSpin->setValue(250);

    fragmentBytesSpin = new QSpinBox();
    fragmentBytesSpin->setRange(4 * 1024, 1024 * 1024);
    fragmentBytesSpin->setValue(64 * 1024);

    programNumberSpin = new QSpinBox();
    programNumberSpin->setRange(0, 65535);
    programNumberSpin->setValue(0);
    programNumberSpin->setSpecialValueText("First program");

    audioCodecCombo = new QComboBox();
    audioCodecCombo->addItem("AAC (libav encoder)", static_cast<int>(AudioCodecPreset::AAC));
    audioCodecCombo->addItem("Opus (libopus)", static_cast<int>(AudioCodecPreset::Opus));

    openh264Check = new QCheckBox("Use OpenH264 H.264 encoder path");
    openh264Check->setChecked(true);
    useLibAvCheck = new QCheckBox("Use direct libav media integration");
    useLibAvCheck->setChecked(true);
    useOpusFallbackCheck = new QCheckBox("Enable libopus fallback");
    useOpusFallbackCheck->setChecked(false);

    form->addRow("MOQ endpoint", endpointEdit);
    form->addRow("MOQ namespace", namespaceEdit);
    form->addRow("Stream name", streamNameEdit);
    form->addRow("M2TS source", videoRow);
    form->addRow("Alternate M2TS source", audioRow);
    form->addRow("Camera source", cameraSourceCombo);
    form->addRow("Microphone source", microphoneSourceCombo);
    form->addRow("", refreshDevicesButton);
    form->addRow("Video width", widthSpin);
    form->addRow("Video height", heightSpin);
    form->addRow("Video frame rate", fpsSpin);
    form->addRow("Video bitrate (kbps)", videoBitrateSpin);
    form->addRow("Audio sample rate", sampleRateSpin);
    form->addRow("Audio channels", channelSpin);
    form->addRow("Audio bitrate (kbps)", audioBitrateSpin);
    form->addRow("Preferred audio codec", audioCodecCombo);
    form->addRow("Fragment duration (ms)", fragmentMsSpin);
    form->addRow("Target segment bytes", fragmentBytesSpin);
    form->addRow("M2TS program", programNumberSpin);
    form->addRow("", openh264Check);
    form->addRow("", useLibAvCheck);
    form->addRow("", useOpusFallbackCheck);

    configLayout->addLayout(form);

    auto* buttonRow = new QHBoxLayout();
    startButton = new QPushButton("Start publishing");
    stopButton = new QPushButton("Stop");
    stopButton->setEnabled(false);
    buttonRow->addWidget(startButton);
    buttonRow->addWidget(stopButton);

    statusLabel = new QLabel("Idle");
    statusLabel->setWordWrap(true);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    logBrowser = new QTextBrowser();
    logBrowser->setOpenExternalLinks(false);
    logBrowser->setPlaceholderText("Logs and pipeline events appear here.");

    configLayout->addLayout(buttonRow);
    configLayout->addWidget(statusLabel);
    configLayout->addStretch(1);
    logsLayout->addWidget(logBrowser);
    tabs->addTab(configTab, QStringLiteral("Config"));
    tabs->addTab(previewPanel, QStringLiteral("Preview"));
    tabs->addTab(logsTab, QStringLiteral("Logs"));
    root->addWidget(tabs);

    QObject::connect(startButton, &QPushButton::clicked, this, &MainWindow::handleStart);
    QObject::connect(stopButton, &QPushButton::clicked, this, &MainWindow::handleStop);
    QObject::connect(cameraSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(microphoneSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(widthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(heightSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(fpsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(sampleRateSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(channelSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);

    refreshCaptureDevices();
    updatePreviewConfig();
    logBrowser->append("Ready to publish.");

    setUiEnabled(true);
}

PreviewPanel* MainWindow::previewPanelWidget() const {
    return previewPanel;
}

void MainWindow::setUiEnabled(bool enabled) {
    endpointEdit->setEnabled(enabled);
    namespaceEdit->setEnabled(enabled);
    streamNameEdit->setEnabled(enabled);
    videoSourceEdit->setEnabled(enabled);
    audioSourceEdit->setEnabled(enabled);
    cameraSourceCombo->setEnabled(enabled);
    microphoneSourceCombo->setEnabled(enabled);
    widthSpin->setEnabled(enabled);
    heightSpin->setEnabled(enabled);
    fpsSpin->setEnabled(enabled);
    videoBitrateSpin->setEnabled(enabled);
    sampleRateSpin->setEnabled(enabled);
    channelSpin->setEnabled(enabled);
    audioBitrateSpin->setEnabled(enabled);
    audioCodecCombo->setEnabled(enabled);
    fragmentMsSpin->setEnabled(enabled);
    fragmentBytesSpin->setEnabled(enabled);
    programNumberSpin->setEnabled(enabled);
    openh264Check->setEnabled(enabled);
    useLibAvCheck->setEnabled(enabled);
    useOpusFallbackCheck->setEnabled(enabled);
    previewPanel->setControlsEnabled(enabled);

    startButton->setEnabled(enabled);
    stopButton->setEnabled(!enabled);
}

void MainWindow::selectVideoSource() {
    const QString filePath = QFileDialog::getOpenFileName(this, "Select video M2TS source", QString(), "M2TS files (*.ts *.m2ts);;All files (*.*)");
    if (!filePath.isEmpty()) {
        videoSourceEdit->setText(filePath);
    }
}

void MainWindow::selectAudioSource() {
    const QString filePath = QFileDialog::getOpenFileName(this, "Select audio M2TS source", QString(), "M2TS files (*.ts *.m2ts);;All files (*.*)");
    if (!filePath.isEmpty()) {
        audioSourceEdit->setText(filePath);
    }
}

void MainWindow::refreshCaptureDevices() {
    cameraSourceCombo->clear();
    microphoneSourceCombo->clear();

    cameraSourceCombo->addItem("No camera selected", QString());
    microphoneSourceCombo->addItem("No microphone selected", QString());

#if defined(Q_OS_DARWIN) || defined(MOQ2TS_HAVE_LIBAV_CAPTURE)
    for (const CaptureDevice& device : LibavCaptureSource::enumerateVideoInputs()) {
        cameraSourceCombo->addItem(device.description, device.id);
    }
    for (const CaptureDevice& device : LibavCaptureSource::enumerateAudioInputs()) {
        microphoneSourceCombo->addItem(device.description, device.id);
    }
#elif defined(MOQ2TS_HAVE_QT_MULTIMEDIA)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto videoInputs = QMediaDevices::videoInputs();
    for (const QCameraDevice& device : videoInputs) {
        cameraSourceCombo->addItem(device.description(), QString::fromUtf8(device.id()));
    }

    const auto audioInputs = QMediaDevices::audioInputs();
    for (const QAudioDevice& device : audioInputs) {
        microphoneSourceCombo->addItem(device.description(), QString::fromUtf8(device.id()));
    }
#else
    const auto cameras = QCameraInfo::availableCameras();
    for (const QCameraInfo& camera : cameras) {
        cameraSourceCombo->addItem(camera.description(), camera.deviceName());
    }

    const auto microphones = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    for (const QAudioDeviceInfo& microphone : microphones) {
        microphoneSourceCombo->addItem(microphone.deviceName(), microphone.deviceName());
    }
#endif
#else
    cameraSourceCombo->setItemText(0, "Qt Multimedia unavailable");
    microphoneSourceCombo->setItemText(0, "Qt Multimedia unavailable");
#endif

    if (cameraSourceCombo->count() == 1) {
        cameraSourceCombo->setItemText(0, "No camera detected");
    }
    if (microphoneSourceCombo->count() == 1) {
        microphoneSourceCombo->setItemText(0, "No microphone detected");
    }
    updatePreviewConfig();
}

PublishConfig MainWindow::currentConfig() const {
    PublishConfig cfg;
    cfg.moqEndpoint = endpointEdit->text().trimmed();
    cfg.namespaceName = namespaceEdit->text().trimmed();
    cfg.streamName = streamNameEdit->text().trimmed();
    cfg.videoSource = videoSourceEdit->text().trimmed();
    cfg.audioSource = audioSourceEdit->text().trimmed();
    cfg.cameraDeviceId = cameraSourceCombo->currentData().toString();
    cfg.microphoneDeviceId = microphoneSourceCombo->currentData().toString();

    cfg.videoWidth = widthSpin->value();
    cfg.videoHeight = heightSpin->value();
    cfg.videoFramerate = fpsSpin->value();
    cfg.videoTargetBitrateKbps = videoBitrateSpin->value();

    cfg.audioSampleRate = sampleRateSpin->value();
    cfg.audioChannels = channelSpin->value();
    cfg.audioTargetBitrateKbps = audioBitrateSpin->value();
    cfg.audioCodec = static_cast<AudioCodecPreset>(audioCodecCombo->currentData().toInt());

    cfg.fragmentDurationMs = fragmentMsSpin->value();
    cfg.targetSegmentBytes = fragmentBytesSpin->value();
    cfg.programNumber = programNumberSpin->value();

    cfg.useOpenh264 = openh264Check->isChecked();
    cfg.useLibAvTranscode = useLibAvCheck->isChecked();
    cfg.useLibOpusFallback = useOpusFallbackCheck->isChecked();

    return cfg;
}

void MainWindow::handleStart() {
    const auto cfg = currentConfig();
    previewPanel->setConfig(cfg);
    previewPanel->stopPreview();

    if (cfg.videoSource.isEmpty() && cfg.audioSource.isEmpty() &&
        cfg.cameraDeviceId.isEmpty() && cfg.microphoneDeviceId.isEmpty()) {
        QMessageBox::warning(this, "Missing source", "Select an M2TS file/pipe or a camera/microphone capture device.");
        return;
    }

    if (cfg.moqEndpoint.isEmpty() || cfg.namespaceName.isEmpty() || cfg.streamName.isEmpty()) {
        QMessageBox::warning(this, "Missing config", "MOQ endpoint, namespace and stream name are required.");
        return;
    }

    setUiEnabled(false);
    statusLabel->setText("Connecting...");
    logBrowser->append(QString("%1 | Publishing started for stream %2/%3").arg(QDateTime::currentDateTime().toString(Qt::ISODate), cfg.namespaceName, cfg.streamName));

    emit startRequested(cfg);
}

void MainWindow::handleStop() {
    emit stopRequested();
    statusLabel->setText("Stopping...");
    logBrowser->append(QString("%1 | Stop requested by user").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
}

void MainWindow::updatePreviewConfig() {
    if (previewPanel) {
        previewPanel->setConfig(currentConfig());
    }
}

void MainWindow::onPublishStatus(const QString& message) {
    statusLabel->setText(message);
    logBrowser->append(QString("%1 | [status] %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), message));
    if (message.contains("ready", Qt::CaseInsensitive) || message.contains("stopped", Qt::CaseInsensitive)) {
        setUiEnabled(true);
    }
}

void MainWindow::onPublishError(const QString& message) {
    statusLabel->setText("Error");
    logBrowser->append(QString("%1 | [error] %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), message));
    QMessageBox::critical(this, "Publish error", message);
    setUiEnabled(true);
}

void MainWindow::onPublishStats(int64_t packets, int64_t bytes) {
    statusLabel->setText(QString("Packets sent: %1, bytes sent: %2").arg(packets).arg(bytes));
}

} // namespace moq2ts
