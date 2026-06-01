#include "LibavPreviewWorker.h"
#include "V4l2Capabilities.h"

#include <QByteArray>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace moq2ts {

namespace {

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
struct AvPacketDeleter {
    void operator()(AVPacket* packet) const {
        av_packet_free(&packet);
    }
};

struct AvFrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

using PacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

QString avError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromLatin1(buffer);
}

PacketPtr makePacket() {
    return PacketPtr(av_packet_alloc());
}

FramePtr makeFrame() {
    return FramePtr(av_frame_alloc());
}

const char* videoInputFormatName() {
#if defined(Q_OS_WIN)
    return "dshow";
#elif defined(Q_OS_DARWIN)
    return "avfoundation";
#else
    return "v4l2";
#endif
}

const char* audioInputFormatName() {
#if defined(Q_OS_WIN)
    return "dshow";
#elif defined(Q_OS_DARWIN)
    return "avfoundation";
#else
    return "pulse";
#endif
}

QString videoInputName(const QString& deviceId) {
#if defined(Q_OS_WIN)
    return QStringLiteral("video=%1").arg(deviceId);
#else
    return deviceId;
#endif
}

QString audioInputName(const QString& deviceId) {
#if defined(Q_OS_WIN)
    return QStringLiteral("audio=%1").arg(deviceId);
#else
    return deviceId;
#endif
}

struct PreviewStream {
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    SwsContext* sws = nullptr;
    int streamIndex = -1;
    bool video = false;
    bool audio = false;

    ~PreviewStream() {
        if (sws) {
            sws_freeContext(sws);
        }
        if (decoder) {
            avcodec_free_context(&decoder);
        }
        if (format) {
            avformat_close_input(&format);
        }
    }
};

bool openStream(const char* formatName,
                const QString& inputName,
                AVMediaType mediaType,
                const PublishConfig& config,
                PreviewStream* stream,
                bool preferMjpeg,
                QString* error) {
    const AVInputFormat* inputFormat = av_find_input_format(formatName);
    if (!inputFormat) {
        if (error) {
            *error = QStringLiteral("libav input format '%1' is unavailable.").arg(QString::fromLatin1(formatName));
        }
        return false;
    }

    AVDictionary* options = nullptr;
    if (mediaType == AVMEDIA_TYPE_VIDEO) {
        av_dict_set(&options, "framerate", QByteArray::number(config.videoFramerate).constData(), 0);
        av_dict_set(&options, "video_size", QStringLiteral("%1x%2").arg(config.videoWidth).arg(config.videoHeight).toUtf8().constData(), 0);
        if (preferMjpeg) {
            av_dict_set(&options, "input_format", "mjpeg", 0);
        }
    }

    AVFormatContext* opened = nullptr;
    int rc = avformat_open_input(&opened, inputName.toUtf8().constData(), inputFormat, &options);
    av_dict_free(&options);
    if (rc < 0) {
        if (error) {
            *error = QStringLiteral("Failed to open preview device '%1': %2").arg(inputName, avError(rc));
        }
        return false;
    }
    stream->format = opened;

    rc = avformat_find_stream_info(stream->format, nullptr);
    if (rc < 0) {
        if (error) {
            *error = QStringLiteral("Failed reading preview stream info: %1").arg(avError(rc));
        }
        return false;
    }

    rc = av_find_best_stream(stream->format, mediaType, -1, -1, nullptr, 0);
    if (rc < 0) {
        if (error) {
            *error = QStringLiteral("No preview %1 stream found.").arg(mediaType == AVMEDIA_TYPE_VIDEO ? QStringLiteral("video") : QStringLiteral("audio"));
        }
        return false;
    }
    stream->streamIndex = rc;
    AVStream* inputStream = stream->format->streams[stream->streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(inputStream->codecpar->codec_id);
    if (!decoder) {
        if (error) {
            *error = QStringLiteral("No decoder found for preview stream.");
        }
        return false;
    }

    stream->decoder = avcodec_alloc_context3(decoder);
    if (!stream->decoder) {
        if (error) {
            *error = QStringLiteral("Failed allocating preview decoder.");
        }
        return false;
    }
    avcodec_parameters_to_context(stream->decoder, inputStream->codecpar);
    rc = avcodec_open2(stream->decoder, decoder, nullptr);
    if (rc < 0) {
        if (error) {
            *error = QStringLiteral("Failed opening preview decoder: %1").arg(avError(rc));
        }
        return false;
    }
    stream->video = mediaType == AVMEDIA_TYPE_VIDEO;
    stream->audio = mediaType == AVMEDIA_TYPE_AUDIO;
    return true;
}

double sampleValue(AVSampleFormat format, const uint8_t* sample) {
    switch (format) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
        return (static_cast<int>(*sample) - 128) / 128.0;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
        return *reinterpret_cast<const int16_t*>(sample) / 32768.0;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        return *reinterpret_cast<const int32_t*>(sample) / 2147483648.0;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
        return *reinterpret_cast<const float*>(sample);
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP:
        return *reinterpret_cast<const double*>(sample);
    default:
        return 0.0;
    }
}

void emitAudioLevels(const FramePtr& frame, LibavPreviewWorker* worker) {
    const auto format = static_cast<AVSampleFormat>(frame->format);
    const int channels = std::max(1, frame->ch_layout.nb_channels);
    const int samples = std::max(0, frame->nb_samples);
    const int bytesPerSample = av_get_bytes_per_sample(format);
    if (samples == 0 || bytesPerSample <= 0) {
        return;
    }

    const bool planar = av_sample_fmt_is_planar(format) != 0;
    double sums[2] = {0.0, 0.0};
    for (int sample = 0; sample < samples; ++sample) {
        for (int outChannel = 0; outChannel < 2; ++outChannel) {
            const int channel = std::min(outChannel, channels - 1);
            const uint8_t* ptr = nullptr;
            if (planar) {
                ptr = frame->extended_data[channel] + sample * bytesPerSample;
            } else {
                ptr = frame->extended_data[0] + ((sample * channels) + channel) * bytesPerSample;
            }
            const double value = sampleValue(format, ptr);
            sums[outChannel] += value * value;
        }
    }

    const double left = std::sqrt(sums[0] / samples);
    const double right = std::sqrt(sums[1] / samples);
    emit worker->audioLevelsChanged(std::clamp(left, 0.0, 1.0), std::clamp(right, 0.0, 1.0));
}

bool processVideoPacket(PreviewStream* stream, AVPacket* packet, LibavPreviewWorker* worker, QString* error) {
    int rc = avcodec_send_packet(stream->decoder, packet);
    if (rc < 0) {
        if (error) {
            *error = QStringLiteral("Failed sending preview video packet: %1").arg(avError(rc));
        }
        return false;
    }

    while (rc >= 0) {
        FramePtr frame = makeFrame();
        rc = avcodec_receive_frame(stream->decoder, frame.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed decoding preview video: %1").arg(avError(rc));
            }
            return false;
        }

        QImage image(frame->width, frame->height, QImage::Format_ARGB32);
        uint8_t* dstData[4] = {image.bits(), nullptr, nullptr, nullptr};
        int dstLinesize[4] = {image.bytesPerLine(), 0, 0, 0};
        stream->sws = sws_getCachedContext(stream->sws,
                                           frame->width,
                                           frame->height,
                                           static_cast<AVPixelFormat>(frame->format),
                                           frame->width,
                                           frame->height,
                                           AV_PIX_FMT_BGRA,
                                           SWS_BILINEAR,
                                           nullptr,
                                           nullptr,
                                           nullptr);
        if (!stream->sws) {
            if (error) {
                *error = QStringLiteral("Failed creating preview video scaler.");
            }
            return false;
        }
        sws_scale(stream->sws, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
        emit worker->videoFrameReady(image.copy());
    }
    return true;
}

bool processAudioPacket(PreviewStream* stream, AVPacket* packet, LibavPreviewWorker* worker, QString* error) {
    int rc = avcodec_send_packet(stream->decoder, packet);
    if (rc < 0) {
        if (error) {
            *error = QStringLiteral("Failed sending preview audio packet: %1").arg(avError(rc));
        }
        return false;
    }

    while (rc >= 0) {
        FramePtr frame = makeFrame();
        rc = avcodec_receive_frame(stream->decoder, frame.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed decoding preview audio: %1").arg(avError(rc));
            }
            return false;
        }
        emitAudioLevels(frame, worker);
    }
    return true;
}

bool readPreviewPacket(PreviewStream* stream, LibavPreviewWorker* worker, QString* error) {
    PacketPtr packet = makePacket();
    const int rc = av_read_frame(stream->format, packet.get());
    if (rc == AVERROR(EAGAIN)) {
        return true;
    }
    if (rc < 0) {
        if (error) {
            *error = QStringLiteral("Failed reading preview device: %1").arg(avError(rc));
        }
        return false;
    }
    if (packet->stream_index != stream->streamIndex) {
        return true;
    }
    if (stream->video) {
        return processVideoPacket(stream, packet.get(), worker, error);
    }
    return processAudioPacket(stream, packet.get(), worker, error);
}
#endif

} // namespace

LibavPreviewWorker::LibavPreviewWorker(QObject* parent)
    : QObject(parent) {}

void LibavPreviewWorker::start(const PublishConfig& config) {
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    emit status(QStringLiteral("Preview starting."));

    std::unique_ptr<PreviewStream> videoStream;
    std::unique_ptr<PreviewStream> audioStream;
    QString openError;
    if (!config.cameraDeviceId.isEmpty()) {
        QString videoNode = config.cameraDeviceId;
        bool preferMjpeg = false;
#if defined(Q_OS_LINUX)
        const CaptureOpen co = resolveCaptureOpen(
            config.cameraDeviceId.toStdString(),
            config.videoWidth, config.videoHeight,
            static_cast<double>(config.videoFramerate));
        videoNode = QString::fromStdString(co.node);
        preferMjpeg = co.useMjpeg;
#endif
        videoStream = std::make_unique<PreviewStream>();
        if (!openStream(videoInputFormatName(), videoInputName(videoNode),
                        AVMEDIA_TYPE_VIDEO, config, videoStream.get(), preferMjpeg, &openError)) {
            // One-time raw fallback when MJPEG open failed; ~PreviewStream frees
            // the failed attempt when the unique_ptr is replaced.
            bool recovered = false;
            if (preferMjpeg) {
                videoStream = std::make_unique<PreviewStream>();
                recovered = openStream(videoInputFormatName(), videoInputName(videoNode),
                                       AVMEDIA_TYPE_VIDEO, config, videoStream.get(), false, &openError);
            }
            if (!recovered) {
                emit error(openError);
                m_running.store(false, std::memory_order_release);
                emit finished();
                return;
            }
        }
    }
    if (!config.microphoneDeviceId.isEmpty()) {
        audioStream = std::make_unique<PreviewStream>();
        if (!openStream(audioInputFormatName(), audioInputName(config.microphoneDeviceId), AVMEDIA_TYPE_AUDIO, config, audioStream.get(), false, &openError)) {
            emit error(openError);
            m_running.store(false, std::memory_order_release);
            emit finished();
            return;
        }
    }

    emit status(QStringLiteral("Preview running."));
    while (m_running.load(std::memory_order_acquire)) {
        QString readError;
        if (videoStream && !readPreviewPacket(videoStream.get(), this, &readError)) {
            emit error(readError);
            break;
        }
        if (audioStream && !readPreviewPacket(audioStream.get(), this, &readError)) {
            emit error(readError);
            break;
        }
        if (!videoStream || !audioStream) {
            QThread::msleep(5);
        }
    }
    m_running.store(false, std::memory_order_release);
    emit status(QStringLiteral("Preview stopped."));
    emit finished();
#else
    Q_UNUSED(config);
    emit error(QStringLiteral("Preview requires a build with libav capture support."));
    emit finished();
#endif
}

void LibavPreviewWorker::stop() {
    m_running.store(false, std::memory_order_release);
}

} // namespace moq2ts
