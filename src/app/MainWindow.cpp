#include "MainWindow.h"

#include <QDateTime>
#include <QHBoxLayout>

namespace moq2ts {

MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle("MOQ M2TS Live Publisher");
    setMinimumSize(720, 560);

    auto* root = new QVBoxLayout(this);

    auto* form = new QFormLayout();

    endpointEdit = new QLineEdit("http://127.0.0.1:9000");
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

    root->addLayout(form);

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

    root->addLayout(buttonRow);
    root->addWidget(statusLabel);
    root->addWidget(logBrowser, 1);

    QObject::connect(startButton, &QPushButton::clicked, this, &MainWindow::handleStart);
    QObject::connect(stopButton, &QPushButton::clicked, this, &MainWindow::handleStop);

    logBrowser->append("Ready to publish.");

    setUiEnabled(true);
}

void MainWindow::setUiEnabled(bool enabled) {
    endpointEdit->setEnabled(enabled);
    namespaceEdit->setEnabled(enabled);
    streamNameEdit->setEnabled(enabled);
    videoSourceEdit->setEnabled(enabled);
    audioSourceEdit->setEnabled(enabled);
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

PublishConfig MainWindow::currentConfig() const {
    PublishConfig cfg;
    cfg.moqEndpoint = endpointEdit->text().trimmed();
    cfg.namespaceName = namespaceEdit->text().trimmed();
    cfg.streamName = streamNameEdit->text().trimmed();
    cfg.videoSource = videoSourceEdit->text().trimmed();
    cfg.audioSource = audioSourceEdit->text().trimmed();

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

    if (cfg.videoSource.isEmpty() && cfg.audioSource.isEmpty()) {
        QMessageBox::warning(this, "Missing source", "At least one of Video or Audio source must be set.");
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
