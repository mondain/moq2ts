#pragma once

#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>

#include "PublishConfig.h"

class QLabel;
class QPushButton;
class QTextBrowser;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;

namespace moq2ts {

class MainWindow final : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

signals:
    void startRequested(const moq2ts::PublishConfig& config);
    void stopRequested();

public slots:
    void onPublishStatus(const QString& message);
    void onPublishError(const QString& message);
    void onPublishStats(int64_t packets, int64_t bytes);

private slots:
    void handleStart();
    void handleStop();
    void selectVideoSource();
    void selectAudioSource();
    void refreshCaptureDevices();

private:
    PublishConfig currentConfig() const;
    void setUiEnabled(bool enabled);

private:
    QLineEdit* endpointEdit = nullptr;
    QLineEdit* namespaceEdit = nullptr;
    QLineEdit* streamNameEdit = nullptr;
    QLineEdit* videoSourceEdit = nullptr;
    QLineEdit* audioSourceEdit = nullptr;
    QComboBox* cameraSourceCombo = nullptr;
    QComboBox* microphoneSourceCombo = nullptr;

    QSpinBox* widthSpin = nullptr;
    QSpinBox* heightSpin = nullptr;
    QSpinBox* fpsSpin = nullptr;
    QSpinBox* videoBitrateSpin = nullptr;
    QSpinBox* sampleRateSpin = nullptr;
    QSpinBox* channelSpin = nullptr;
    QSpinBox* audioBitrateSpin = nullptr;
    QSpinBox* fragmentMsSpin = nullptr;
    QSpinBox* fragmentBytesSpin = nullptr;
    QSpinBox* programNumberSpin = nullptr;

    QComboBox* audioCodecCombo = nullptr;

    QCheckBox* openh264Check = nullptr;
    QCheckBox* useLibAvCheck = nullptr;
    QCheckBox* useOpusFallbackCheck = nullptr;

    QPushButton* startButton = nullptr;
    QPushButton* stopButton = nullptr;
    QLabel* statusLabel = nullptr;
    QTextBrowser* logBrowser = nullptr;
};

} // namespace moq2ts
