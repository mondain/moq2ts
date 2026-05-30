#include <QApplication>
#include <QMetaObject>

#include <chrono>
#include <future>

#include "app/MainWindow.h"
#include "media/LivePipeline.h"
#include "publish/MoqxrPublisher.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("moq2ts");
    QCoreApplication::setApplicationName("moq2ts-publisher");

    moq2ts::MainWindow window;
    moq2ts::MoqxrPublisher publisher;
    moq2ts::LivePipeline pipeline;
    std::future<void> shutdownFuture;

    QObject::connect(&window, &moq2ts::MainWindow::startRequested, [&](const moq2ts::PublishConfig& cfg) {
        if (!publisher.connect(cfg)) {
            window.onPublishError("Could not open MOQ publish session.");
            return;
        }

        pipeline.start(cfg, &publisher);
    });

    QObject::connect(&window, &moq2ts::MainWindow::stopRequested, [&]() {
        pipeline.requestStop();
        if (shutdownFuture.valid() &&
            shutdownFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        if (shutdownFuture.valid()) {
            shutdownFuture.get();
        }
        shutdownFuture = std::async(std::launch::async, [&]() {
            publisher.stop();
            pipeline.stop();
            QMetaObject::invokeMethod(&window, [&window]() {
                window.onPublishStatus("Stopped by user");
            }, Qt::QueuedConnection);
        });
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        publisher.stop();
        pipeline.requestStop();
        pipeline.waitForStopped();
        if (shutdownFuture.valid()) {
            shutdownFuture.wait();
        }
    });

    QObject::connect(&pipeline, &moq2ts::LivePipeline::status, &window, &moq2ts::MainWindow::onPublishStatus);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::error, &window, &moq2ts::MainWindow::onPublishError);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::stats, &window, &moq2ts::MainWindow::onPublishStats);
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::framePublished,
                     &window, &moq2ts::MainWindow::onPublisherFramePublished);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::previewVideoFrame, window.previewPanelWidget(), &moq2ts::PreviewPanel::handleVideoFrame);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::previewAudioLevels, window.previewPanelWidget(), &moq2ts::PreviewPanel::handleAudioLevels);
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::connectionStateChanged, &window,
                     [&](bool connected, const QString& message) {
                         Q_UNUSED(connected);
                         window.onPublishStatus(message);
                     });
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::publishError,
                     &window, &moq2ts::MainWindow::onPublishError);

    window.show();
    return app.exec();
}
