#include <QApplication>

#include "app/MainWindow.h"
#include "media/LivePipeline.h"
#include "publish/MoqxrPublisher.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    moq2ts::MainWindow window;
    moq2ts::MoqxrPublisher publisher;
    moq2ts::LivePipeline pipeline;

    QObject::connect(&window, &moq2ts::MainWindow::startRequested, [&](const moq2ts::PublishConfig& cfg) {
        if (!publisher.connect(cfg)) {
            window.onPublishError("Could not open MOQ publish session.");
            return;
        }

        pipeline.start(cfg, &publisher);
    });

    QObject::connect(&window, &moq2ts::MainWindow::stopRequested, [&]() {
        pipeline.stop();
        publisher.stop();
        window.onPublishStatus("Stopped by user");
    });

    QObject::connect(&pipeline, &moq2ts::LivePipeline::status, &window, &moq2ts::MainWindow::onPublishStatus);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::error, &window, &moq2ts::MainWindow::onPublishError);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::stats, &window, &moq2ts::MainWindow::onPublishStats);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::previewVideoFrame, window.previewPanelWidget(), &moq2ts::PreviewPanel::handleVideoFrame);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::previewAudioLevels, window.previewPanelWidget(), &moq2ts::PreviewPanel::handleAudioLevels);
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::connectionStateChanged,
                     [&](bool connected, const QString& message) {
                         Q_UNUSED(connected);
                         window.onPublishStatus(message);
                     });
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::publishError,
                     &window, &moq2ts::MainWindow::onPublishError);

    window.show();
    return app.exec();
}
