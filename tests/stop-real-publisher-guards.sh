#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

main_cpp="$repo_root/src/main.cpp"
publisher_cpp="$repo_root/src/publish/MoqxrPublisher.cpp"
publisher_h="$repo_root/src/publish/MoqxrPublisher.h"
pipeline_h="$repo_root/src/media/LivePipeline.h"

python3 - "$main_cpp" "$publisher_cpp" "$publisher_h" "$pipeline_h" <<'PY'
from pathlib import Path
import sys

main_cpp = Path(sys.argv[1]).read_text()
publisher_cpp = Path(sys.argv[2]).read_text()
publisher_h = Path(sys.argv[3]).read_text()
pipeline_h = Path(sys.argv[4]).read_text()

stop_block = main_cpp.split("QObject::connect(&window, &moq2ts::MainWindow::stopRequested", 1)[1].split("});", 1)[0]
if "shutdownFuture" not in main_cpp:
    raise SystemExit("main must own a shutdown future for nonblocking Stop")
if "std::async(std::launch::async" not in stop_block:
    raise SystemExit("stopRequested handler must move blocking shutdown work off the UI thread")
if stop_block.find("publisher.stop();") == -1 or stop_block.find("pipeline.stop();") == -1:
    raise SystemExit("background stop handler must stop publisher and pipeline")
if stop_block.find("publisher.stop();") > stop_block.find("pipeline.stop();"):
    raise SystemExit("publisher.stop() must run before pipeline.requestStop() so blocking real publishes can be interrupted")
if "QMetaObject::invokeMethod(&window" not in stop_block:
    raise SystemExit("background stop handler must marshal UI status updates back to the window thread")

if "QCoreApplication::aboutToQuit" not in main_cpp:
    raise SystemExit("application shutdown must stop publisher before pipeline destructors run")
quit_block = main_cpp.split("QCoreApplication::aboutToQuit", 1)[1].split("});", 1)[0]
if quit_block.find("publisher.stop();") == -1 or quit_block.find("pipeline.requestStop();") == -1 or quit_block.find("pipeline.waitForStopped();") == -1:
    raise SystemExit("aboutToQuit handler must stop publisher and pipeline")
if quit_block.find("publisher.stop();") > quit_block.find("pipeline.requestStop();"):
    raise SystemExit("aboutToQuit must stop publisher before pipeline")
if "shutdownFuture.wait" not in quit_block:
    raise SystemExit("aboutToQuit must wait for any background shutdown task (bounded)")

if "pipeline.requestStop();" not in main_cpp:
    raise SystemExit("stopRequested handler must request pipeline stop without joining on the UI thread")
if "pipeline.waitForStopped();" not in main_cpp:
    raise SystemExit("aboutToQuit handler must wait for pipeline shutdown before destructors run")

required = [
    "void requestStop();",
    "void waitForStopped();",
    "std::shared_ptr<openmoq::publisher::Publisher> m_activePublisher",
    "auto activePublisher = std::make_shared<openmoq::publisher::Publisher>",
    "m_activePublisher = activePublisher",
    "m_activePublisher.reset()",
    "activePublisher->publish_live_objects",
    "activePublisher->disconnect(0)",
    "activePublisher->stats()",
    "publisherToStop->disconnect(0)",
]
for needle in required:
    if needle not in publisher_cpp and needle not in publisher_h and needle not in pipeline_h:
        raise SystemExit(f"missing shutdown guard pattern: {needle}")
PY
