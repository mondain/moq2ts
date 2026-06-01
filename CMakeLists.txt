cmake_minimum_required(VERSION 3.24)

project(moq2ts-ui
    VERSION 0.1.0
    DESCRIPTION "Cross-platform MOQ live publisher for H.264/AAC-Opus M2TS"
    LANGUAGES CXX)

option(MOQ2TS_BUILD_WITH_MOCK_MOQXR "Build without linking to a moqxr SDK and use the mock publisher adapter" OFF)
option(MOQ2TS_ENABLE_OPENH264 "Enable OpenH264 integration for H.264 encoding" ON)
option(MOQ2TS_ENABLE_LIBAV_AUDIO "Enable libavcodec/libavformat integration" ON)
option(MOQ2TS_ENABLE_LIBOPUS "Enable libopus fallback audio encoder path" ON)
set(MOQXR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../moqxr" CACHE PATH "Path to local moqxr checkout")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------- Qt discovery ----------------
set(QT_LIBS)
if(NOT QT_DEFAULT_MAJOR_VERSION)
    set(QT_OPTIONAL_COMPONENTS)
    if(WIN32)
        list(APPEND QT_OPTIONAL_COMPONENTS Multimedia)
    endif()

    find_package(Qt6 QUIET COMPONENTS Widgets OPTIONAL_COMPONENTS ${QT_OPTIONAL_COMPONENTS})
    if(Qt6_FOUND)
        list(APPEND QT_LIBS Qt6::Widgets)
        if(TARGET Qt6::Multimedia)
            list(APPEND QT_LIBS Qt6::Multimedia)
            add_compile_definitions(MOQ2TS_HAVE_QT_MULTIMEDIA)
        endif()
    else()
        find_package(Qt5 QUIET COMPONENTS Widgets OPTIONAL_COMPONENTS ${QT_OPTIONAL_COMPONENTS})
        if(Qt5_FOUND)
            list(APPEND QT_LIBS Qt5::Widgets)
            if(TARGET Qt5::Multimedia)
                list(APPEND QT_LIBS Qt5::Multimedia)
                add_compile_definitions(MOQ2TS_HAVE_QT_MULTIMEDIA)
            endif()
        endif()
    endif()
endif()

if(NOT QT_LIBS)
    message(FATAL_ERROR "Qt Widgets was not found. Install Qt5/Qt6 development packages before building.")
endif()

# ---------------- External codec dependencies ----------------

# openh264
if(MOQ2TS_ENABLE_OPENH264)
    find_path(OPENH264_INCLUDE_DIR wels/codec_api.h)
    find_library(OPENH264_LIBRARY NAMES openh264)
    if(OPENH264_INCLUDE_DIR AND OPENH264_LIBRARY)
        set(OPENH264_FOUND TRUE)
        add_library(openh264::openh264 UNKNOWN IMPORTED)
        set_target_properties(openh264::openh264 PROPERTIES
            IMPORTED_LOCATION "${OPENH264_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENH264_INCLUDE_DIR}")
        add_compile_definitions(MOQ2TS_HAVE_OPENH264)
    else()
        set(OPENH264_FOUND FALSE)
        message(WARNING "OpenH264 not found. UI and packetization will still work; add libopenh264-dev or equivalent to enable full encode path.")
    endif()
endif()

# libav via pkg-config
if(MOQ2TS_ENABLE_LIBAV_AUDIO)
    include(FindPkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBAVCODEC QUIET IMPORTED_TARGET libavcodec)
        pkg_check_modules(LIBAVDEVICE QUIET IMPORTED_TARGET libavdevice)
        pkg_check_modules(LIBAVFORMAT QUIET IMPORTED_TARGET libavformat)
        pkg_check_modules(LIBAVUTIL QUIET IMPORTED_TARGET libavutil)
        pkg_check_modules(LIBSWRESAMPLE QUIET IMPORTED_TARGET libswresample)
        pkg_check_modules(LIBSWSCALE QUIET IMPORTED_TARGET libswscale)
    endif()
    if(LIBAVCODEC_FOUND AND LIBAVFORMAT_FOUND AND LIBAVUTIL_FOUND)
        set(FFMPEG_FOUND TRUE)
        add_compile_definitions(MOQ2TS_HAVE_LIBAV)
        if(LIBAVDEVICE_FOUND AND LIBSWRESAMPLE_FOUND AND LIBSWSCALE_FOUND)
            add_compile_definitions(MOQ2TS_HAVE_LIBAV_CAPTURE)
        endif()
    else()
        set(FFMPEG_FOUND FALSE)
        message(WARNING "libav libraries were not found via pkg-config. Install libavcodec-dev/libavformat-dev/libavutil-dev for optional transcoding support.")
    endif()
endif()

# libopus (optional)
if(MOQ2TS_ENABLE_LIBOPUS)
    find_library(OPUS_LIBRARY NAMES opus libopus)
    find_path(OPUS_INCLUDE_DIR opus/opus.h)
    if(OPUS_LIBRARY AND OPUS_INCLUDE_DIR)
        set(OPUS_FOUND TRUE)
        add_library(opus::opus UNKNOWN IMPORTED)
        set_target_properties(opus::opus PROPERTIES
            IMPORTED_LOCATION "${OPUS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INCLUDE_DIR}")
        add_compile_definitions(MOQ2TS_HAVE_LIBOPUS)
    else()
        set(OPUS_FOUND FALSE)
        message(WARNING "libopus not found. Opus encode path will be unavailable.")
    endif()
endif()

# moqxr API discovery.
set(MOQXR_PUBLISH_INCLUDE_DIR_HINT "${MOQXR_SOURCE_DIR}/include" CACHE PATH "Optional moqxr include directory")
find_path(MOQXR_PUBLISH_INCLUDE_DIR
    NAMES openmoq/publisher/publisher_api.h
    PATHS ${MOQXR_PUBLISH_INCLUDE_DIR_HINT} /usr/include /usr/local/include /opt/include
)
if(MOQXR_PUBLISH_INCLUDE_DIR)
    set(MOQXR_PUBLISH_FOUND TRUE)
else()
    set(MOQXR_PUBLISH_FOUND FALSE)
    if(NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR)
        message(FATAL_ERROR "moqxr publish headers not found. Set MOQ2TS_BUILD_WITH_MOCK_MOQXR=ON or provide MOQXR_SOURCE_DIR/MOQXR_PUBLISH_INCLUDE_DIR_HINT.")
    endif()
    message(STATUS "moqxr headers not found. Building mock publisher adapter.")
endif()

if(MOQXR_PUBLISH_FOUND AND NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR)
    add_compile_definitions(MOQ2TS_HAS_MOQXR)
    if(EXISTS "${MOQXR_SOURCE_DIR}/CMakeLists.txt" AND NOT TARGET openmoq_publisher_lib)
        set(OPENMOQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${MOQXR_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/moqxr" EXCLUDE_FROM_ALL)
    endif()
endif()

set(SOURCES
    src/main.cpp
    src/app/MainWindow.cpp
    src/app/PreviewPanel.cpp
    src/media/LivePipeline.cpp
    src/media/LibavCaptureSource.cpp
    src/media/V4l2Capabilities.cpp
    src/media/LibavPreviewWorker.cpp
    src/media/M2tsPacketizer.cpp
    src/media/MsftsMuxer.cpp
    src/publish/MoqxrPublisher.cpp
)

if(APPLE)
    list(APPEND SOURCES src/media/MacCaptureEnumerator.mm)
    enable_language(OBJCXX)
endif()

set(HEADERS
    src/app/MainWindow.h
    src/app/PreviewPanel.h
    src/app/PublishConfig.h
    src/media/LivePipeline.h
    src/media/LibavCaptureSource.h
    src/media/V4l2Capabilities.h
    src/media/LibavPreviewWorker.h
    src/media/M2tsPacketizer.h
    src/media/MsftsMuxer.h
    src/publish/MoqxrPublisher.h
)

add_executable(moq2ts-publisher
    ${SOURCES}
    ${HEADERS}
)

if(APPLE)
    set_target_properties(moq2ts-publisher PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "moq2ts"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.moq2ts.publisher"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) moq2ts contributors"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in"
    )
endif()

set_target_properties(moq2ts-publisher PROPERTIES AUTOGEN_BUILD_DIR ${CMAKE_BINARY_DIR}/generated)

set_target_properties(moq2ts-publisher PROPERTIES
    AUTOMOC TRUE
    AUTOUIC TRUE
    AUTORCC TRUE
)

target_include_directories(moq2ts-publisher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(moq2ts-publisher PRIVATE ${QT_LIBS})

if(APPLE)
    target_link_libraries(moq2ts-publisher PRIVATE
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework Foundation"
    )
endif()

if(OPENH264_FOUND)
    target_link_libraries(moq2ts-publisher PRIVATE openh264::openh264)
endif()

if(FFMPEG_FOUND)
    target_include_directories(moq2ts-publisher PRIVATE ${LIBAVCODEC_INCLUDE_DIRS} ${LIBAVDEVICE_INCLUDE_DIRS} ${LIBAVFORMAT_INCLUDE_DIRS} ${LIBAVUTIL_INCLUDE_DIRS} ${LIBSWRESAMPLE_INCLUDE_DIRS} ${LIBSWSCALE_INCLUDE_DIRS})
    target_link_libraries(moq2ts-publisher PRIVATE PkgConfig::LIBAVCODEC PkgConfig::LIBAVFORMAT PkgConfig::LIBAVUTIL)
    if(LIBAVDEVICE_FOUND)
        target_link_libraries(moq2ts-publisher PRIVATE PkgConfig::LIBAVDEVICE)
    endif()
    if(LIBSWRESAMPLE_FOUND)
        target_link_libraries(moq2ts-publisher PRIVATE PkgConfig::LIBSWRESAMPLE)
    endif()
    if(LIBSWSCALE_FOUND)
        target_link_libraries(moq2ts-publisher PRIVATE PkgConfig::LIBSWSCALE)
    endif()
endif()

if(OPUS_FOUND)
    target_link_libraries(moq2ts-publisher PRIVATE opus::opus)
endif()

if(MOQXR_PUBLISH_FOUND)
    target_include_directories(moq2ts-publisher PRIVATE ${MOQXR_PUBLISH_INCLUDE_DIR})
endif()

if(NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR AND TARGET openmoq_publisher_lib)
    target_link_libraries(moq2ts-publisher PRIVATE openmoq_publisher_lib)
elseif(NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR)
    message(FATAL_ERROR "MOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF requires a local moqxr checkout exposing openmoq_publisher_lib. Set MOQXR_SOURCE_DIR.")
endif()

install(TARGETS moq2ts-publisher
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

# ----------------
# Optional CLI target intentionally omitted until a standalone CLI entrypoint file
# is added (kept as a placeholder for future expansion).

# Pure V4L2 mode-selection logic test (Linux only; no Qt/libav deps).
if(UNIX AND NOT APPLE)
    enable_testing()
    add_executable(moq2ts-v4l2-selection-tests
        tests/v4l2_selection_test.cpp
        src/media/V4l2Capabilities.cpp
    )
    target_include_directories(moq2ts-v4l2-selection-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME moq2ts-v4l2-selection-tests COMMAND moq2ts-v4l2-selection-tests)
endif()

# Pure egress-pacing delay logic test (no Qt/libav deps).
if(UNIX AND NOT APPLE)
    enable_testing()
    add_executable(moq2ts-pace-delay-tests
        tests/pace_delay_test.cpp
    )
    target_include_directories(moq2ts-pace-delay-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME moq2ts-pace-delay-tests COMMAND moq2ts-pace-delay-tests)
endif()
