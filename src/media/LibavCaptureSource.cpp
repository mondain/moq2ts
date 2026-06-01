#include "LibavCaptureSource.h"

#include "V4l2Capabilities.h"

#include "EgressPacing.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <QImage>
#include <QSet>
#include <QtGlobal>

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace moq2ts {

namespace {

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE

QString avError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

// libswscale logs "deprecated pixel format used, make sure you did set range
// correctly" whenever a full-range yuvj* enum reaches it -- and it logs this at
// context creation, before any sws_setColorspaceDetails call. Map yuvj* to its
// non-deprecated equivalent (identical layout) for the scaler's source format,
// then carry the full range explicitly via applyScalerRange. This silences the
// warning at its source while keeping the range conversion correct.
AVPixelFormat dejpegFormat(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default: return format;
    }
}

// Tell swscale the source/destination range. srcRangeFull reflects the original
// (pre-dejpeg) source pixel format; dstRangeFull reflects the destination.
void applyScalerRange(SwsContext* sws, AVPixelFormat srcFormat, bool dstRangeFull) {
    if (!sws) {
        return;
    }
    const bool srcRangeFull = srcFormat == AV_PIX_FMT_YUVJ420P ||
                              srcFormat == AV_PIX_FMT_YUVJ422P ||
                              srcFormat == AV_PIX_FMT_YUVJ444P ||
                              srcFormat == AV_PIX_FMT_YUVJ440P;
    int* invTable = nullptr;
    int* table = nullptr;
    int srcRange = 0;
    int dstRange = 0;
    int brightness = 0;
    int contrast = 0;
    int saturation = 0;
    if (sws_getColorspaceDetails(sws, &invTable, &srcRange, &table, &dstRange,
                                 &brightness, &contrast, &saturation) < 0) {
        return;
    }
    sws_setColorspaceDetails(sws, invTable, srcRangeFull ? 1 : 0,
                             table, dstRangeFull ? 1 : 0,
                             brightness, contrast, saturation);
}

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
#elif defined(Q_OS_DARWIN)
    return deviceId;
#else
    return deviceId;
#endif
}

QString audioInputName(const QString& deviceId) {
#if defined(Q_OS_WIN)
    return QStringLiteral("audio=%1").arg(deviceId);
#elif defined(Q_OS_DARWIN)
    return QStringLiteral(":%1").arg(deviceId);
#else
    return deviceId;
#endif
}

int writePacket(void* opaque, uint8_t* buffer, int bufferSize) {
    auto* output = static_cast<QByteArray*>(opaque);
    output->append(reinterpret_cast<const char*>(buffer), bufferSize);
    return bufferSize;
}

int readPid(const QByteArray& packet) {
    if (packet.size() < 4) {
        return -1;
    }
    return ((static_cast<unsigned char>(packet[1]) & 0x1f) << 8) |
           static_cast<unsigned char>(packet[2]);
}

bool extractInitData(const QByteArray& bytes, QByteArray* initData, int* pmtPid, int* pcrPid) {
    QByteArray pat;
    QByteArray pmt;
    for (int offset = 0; offset + 188 <= bytes.size(); offset += 188) {
        const QByteArray packet = bytes.mid(offset, 188);
        if (static_cast<unsigned char>(packet[0]) != 0x47) {
            continue;
        }
        const int pid = readPid(packet);
        if (pid == 0 && pat.isEmpty()) {
            pat = packet;
            const int payloadOffset = 4;
            const int pointer = static_cast<unsigned char>(packet[payloadOffset]);
            const int section = payloadOffset + 1 + pointer;
            if (section + 12 <= packet.size()) {
                const int programInfo = section + 8;
                *pmtPid = ((static_cast<unsigned char>(packet[programInfo + 2]) & 0x1f) << 8) |
                          static_cast<unsigned char>(packet[programInfo + 3]);
            }
        } else if (*pmtPid >= 0 && pid == *pmtPid && pmt.isEmpty()) {
            pmt = packet;
            const int payloadOffset = 4;
            const int pointer = static_cast<unsigned char>(packet[payloadOffset]);
            const int section = payloadOffset + 1 + pointer;
            if (section + 12 <= packet.size()) {
                *pcrPid = ((static_cast<unsigned char>(packet[section + 8]) & 0x1f) << 8) |
                          static_cast<unsigned char>(packet[section + 9]);
            }
        }
        if (!pat.isEmpty() && !pmt.isEmpty()) {
            *initData = pat + pmt;
            return true;
        }
    }
    return false;
}

#endif

} // namespace

struct LibavCaptureSource::Impl {
    explicit Impl(PublishConfig cfg)
        : config(std::move(cfg)) {}

    PublishConfig config;
    QByteArray muxedBytes;
    QByteArray initDataBytes;
    std::uint64_t nextObjectId = 0;
    int pmtPidValue = -1;
    int pcrPidValue = -1;
    // Absolute byte offset (since stream start) at which each MOQT group begins.
    // A new group starts where a keyframe's muxed TS packets begin. Consumed in
    // order by readObject. Front entry is the next pending boundary.
    std::deque<std::uint64_t> groupBoundaries;
    std::uint64_t muxedConsumed = 0;
    std::uint64_t nextGroupId = 0;
    bool sawVideoKeyframe = false;
    bool hasVideoStream = false;
    std::uint64_t nextObjectIdInGroup = 0;
    std::uint64_t pendingGroupBoundary = 0;  // absolute offset of the next group start, or 0 if none
    std::uint64_t rapBoundaryCount = 0;  // diagnostic: number of RAP boundaries recorded

    // Single capture epoch (steady-clock microseconds) shared by video and audio
    // so both PES streams and the mpegts PCR sit on one timeline. -1 until set
    // by the first encoded frame of either stream (CAS, set-once).
    std::atomic<std::int64_t> captureEpochUs{-1};
    // Ascending (muxedByteOffset, videoMediaUs) pairs: media time of the latest
    // video packet whose bytes end at/before each offset. Consumed by readObject.
    std::deque<std::pair<std::uint64_t, std::uint64_t>> offsetMediaUs;
    // Carries the media time of the last video packet consumed by readObject so
    // an object with no new video bytes reuses the last known media time.
    std::uint64_t lastVideoMediaUs = 0;

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    struct StreamState {
        AVFormatContext* inputFormat = nullptr;
        AVCodecContext* decoder = nullptr;
        AVCodecContext* encoder = nullptr;
        AVStream* inputStream = nullptr;
        AVStream* outputStream = nullptr;
        AVAudioFifo* audioFifo = nullptr;
        SwsContext* sws = nullptr;
        SwsContext* previewSws = nullptr;
        SwrContext* swr = nullptr;
        int64_t nextAudioPts = 0;
        bool audioAnchored = false;
        int inputStreamIndex = -1;
        bool audio = false;
        bool video = false;
    };

    std::vector<std::unique_ptr<StreamState>> streams;
    AVFormatContext* outputFormat = nullptr;
    AVIOContext* outputIo = nullptr;
    unsigned char* outputIoBuffer = nullptr;
    bool headerWritten = false;
    std::function<void(const QImage&)> videoFrameCallback;
    std::function<void(double, double)> audioLevelsCallback;

    ~Impl() {
        if (outputFormat && headerWritten) {
            av_write_trailer(outputFormat);
        }
        if (outputIo) {
            avio_context_free(&outputIo);
        } else if (outputIoBuffer) {
            av_free(outputIoBuffer);
        }
        if (outputFormat) {
            avformat_free_context(outputFormat);
        }
        for (auto& stream : streams) {
            if (stream->sws) {
                sws_freeContext(stream->sws);
            }
            if (stream->previewSws) {
                sws_freeContext(stream->previewSws);
            }
            if (stream->swr) {
                swr_free(&stream->swr);
            }
            if (stream->audioFifo) {
                av_audio_fifo_free(stream->audioFifo);
            }
            if (stream->decoder) {
                avcodec_free_context(&stream->decoder);
            }
            if (stream->encoder) {
                avcodec_free_context(&stream->encoder);
            }
            if (stream->inputFormat) {
                avformat_close_input(&stream->inputFormat);
            }
        }
    }

    bool open(QString* error) {
        avdevice_register_all();

        int rc = avformat_alloc_output_context2(&outputFormat, nullptr, "mpegts", nullptr);
        if (rc < 0 || !outputFormat) {
            if (error) {
                *error = QStringLiteral("Failed to allocate MPEG-TS muxer: %1").arg(avError(rc));
            }
            return false;
        }

        constexpr int ioBufferSize = 32768;
        outputIoBuffer = static_cast<unsigned char*>(av_malloc(ioBufferSize));
        outputIo = avio_alloc_context(outputIoBuffer, ioBufferSize, 1, &muxedBytes, nullptr, writePacket, nullptr);
        if (!outputIo) {
            if (error) {
                *error = QStringLiteral("Failed to allocate MPEG-TS memory IO.");
            }
            return false;
        }
        outputIoBuffer = nullptr;
        outputFormat->pb = outputIo;
        outputFormat->flags |= AVFMT_FLAG_CUSTOM_IO;

        if (!config.cameraDeviceId.isEmpty() && !addVideo(error)) {
            return false;
        }
        if (!config.microphoneDeviceId.isEmpty() && !addAudio(error)) {
            return false;
        }
        if (streams.empty()) {
            if (error) {
                *error = QStringLiteral("No capture streams were selected.");
            }
            return false;
        }

        rc = avformat_write_header(outputFormat, nullptr);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to write MPEG-TS header: %1").arg(avError(rc));
            }
            return false;
        }
        headerWritten = true;

        // One-shot pipeline confirmation: for each stream, report how the camera
        // input is decoded, what it is re-encoded to, and the container it is
        // muxed into before MOQT packaging. Makes the decode->encode->mux path
        // explicit in the logs (e.g. "decode=mjpeg -> encode=h264 -> mux=mpegts").
        for (const auto& stream : streams) {
            const char* decodeName =
                (stream->decoder && stream->decoder->codec) ? stream->decoder->codec->name : "?";
            const char* encodeName =
                (stream->encoder && stream->encoder->codec) ? stream->encoder->codec->name : "?";
            const char* muxName =
                (outputFormat && outputFormat->oformat) ? outputFormat->oformat->name : "?";
            std::fprintf(stderr, "[moqxr][pipeline] %s: decode=%s -> encode=%s -> mux=%s\n",
                         stream->video ? "video" : "audio", decodeName, encodeName, muxName);
        }
        std::fflush(stderr);

        extractInitData(muxedBytes, &initDataBytes, &pmtPidValue, &pcrPidValue);
        pumpUntilInitData();
        return true;
    }

    void setPreviewCallbacks(std::function<void(const QImage&)> videoCallback,
                             std::function<void(double, double)> audioCallback) {
        videoFrameCallback = std::move(videoCallback);
        audioLevelsCallback = std::move(audioCallback);
    }

    bool addVideo(QString* error) {
        auto stream = std::make_unique<StreamState>();
        stream->video = true;

        // Choose the best (node, pixel format) for the requested geometry.
        QString chosenNode = config.cameraDeviceId;
        bool useMjpeg = false;
#if defined(Q_OS_LINUX)
        {
            const CaptureOpen co = resolveCaptureOpen(
                config.cameraDeviceId.toStdString(),
                config.videoWidth, config.videoHeight,
                static_cast<double>(config.videoFramerate));
            chosenNode = QString::fromStdString(co.node);
            useMjpeg = co.useMjpeg;
            std::fprintf(stderr, "[moqxr][capture] selected node=%s mjpeg=%d targetFps=%d\n",
                         chosenNode.toUtf8().constData(), useMjpeg ? 1 : 0, config.videoFramerate);
        }
#endif

        const auto openWith = [&](bool mjpeg, QString* openError) -> bool {
            AVDictionary* options = nullptr;
            av_dict_set(&options, "framerate", QByteArray::number(config.videoFramerate).constData(), 0);
            av_dict_set(&options, "video_size",
                        QStringLiteral("%1x%2").arg(config.videoWidth).arg(config.videoHeight).toUtf8().constData(), 0);
            if (mjpeg) {
                av_dict_set(&options, "input_format", "mjpeg", 0);
            }
            const bool opened = openInput(videoInputFormatName(),
                                          videoInputName(chosenNode),
                                          AVMEDIA_TYPE_VIDEO, stream.get(), &options, openError);
            av_dict_free(&options);
            return opened;
        };

        QString openError;
        if (!openWith(useMjpeg, &openError)) {
            // A failed open may have left a partially-initialized input context
            // (e.g. open succeeded but stream-info/decoder setup failed). Tear it
            // down before retrying so the raw open does not overwrite and leak it.
            if (useMjpeg) {
                if (stream->decoder) {
                    avcodec_free_context(&stream->decoder);
                }
                if (stream->inputFormat) {
                    avformat_close_input(&stream->inputFormat);
                }
                stream->inputStream = nullptr;
                stream->inputStreamIndex = -1;
            }
            // If MJPEG open failed, retry once with raw before giving up.
            if (useMjpeg && openWith(false, error)) {
                std::fprintf(stderr, "[moqxr][capture] MJPEG open failed (%s); fell back to raw\n",
                             openError.toUtf8().constData());
            } else {
                if (error && error->isEmpty()) {
                    *error = openError;
                }
                return false;
            }
        }

        const AVCodec* encoder = avcodec_find_encoder_by_name("libopenh264");
        if (!encoder) {
            encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        if (!encoder) {
            if (error) {
                *error = QStringLiteral("No H.264 encoder found. Install libopenh264/libavcodec H.264 support.");
            }
            return false;
        }

        stream->encoder = avcodec_alloc_context3(encoder);
        stream->encoder->codec_id = encoder->id;
        stream->encoder->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->encoder->width = config.videoWidth;
        stream->encoder->height = config.videoHeight;
        // Derive the encoder rate from what the device actually negotiated, so
        // timestamps and GOP length match reality even when the driver could not
        // honor the requested rate (e.g. a raw mode capped below target).
        AVRational negRate = stream->inputStream->avg_frame_rate;
        if (negRate.num <= 0 || negRate.den <= 0) {
            negRate = stream->inputStream->r_frame_rate;
        }
        int negFps = (negRate.num > 0 && negRate.den > 0)
                         ? std::max(1, static_cast<int>(std::lround(av_q2d(negRate))))
                         : std::max(1, config.videoFramerate);
        if (negFps != config.videoFramerate) {
            std::fprintf(stderr,
                         "[moqxr][capture] requested %d fps, capturing at %d fps\n",
                         config.videoFramerate, negFps);
        }

        stream->encoder->time_base = AVRational{1, negFps};
        stream->encoder->framerate = AVRational{negFps, 1};
        stream->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        stream->encoder->bit_rate = static_cast<int64_t>(config.videoTargetBitrateKbps) * 1000;
        // Bounded, closed GOP so the live stream emits periodic IDRs (every
        // keyframeIntervalMs). Without this the encoder uses an effectively long
        // default GOP, no random-access points appear after the first frame, and
        // MOQT grouping degenerates to a single group.
        const int frameRate = std::max(1, negFps);
        int gopSize = static_cast<int>(
            (static_cast<long long>(frameRate) * std::max(1, config.keyframeIntervalMs) + 500) / 1000);
        gopSize = std::max(1, gopSize);
        stream->encoder->gop_size = gopSize;
        stream->encoder->max_b_frames = 0;  // closed, low-latency GOP
        av_opt_set(stream->encoder->priv_data, "profile", "baseline", 0);
        // Belt-and-suspenders for the libx264 fallback: pin keyint and disable
        // scene-cut so IDRs land on a fixed period. These options do not exist on
        // libopenh264 (the primary encoder), which honors gop_size directly; the
        // return codes are intentionally ignored so an absent option is not fatal.
        av_opt_set_int(stream->encoder->priv_data, "keyint", gopSize, 0);
        av_opt_set_int(stream->encoder->priv_data, "min-keyint", gopSize, 0);
        av_opt_set(stream->encoder->priv_data, "sc_threshold", "0", 0);
        int rc = avcodec_open2(stream->encoder, encoder, nullptr);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to open H.264 encoder: %1").arg(avError(rc));
            }
            return false;
        }
        hasVideoStream = true;
        return addOutputStream(std::move(stream), error);
    }

    bool addAudio(QString* error) {
        auto stream = std::make_unique<StreamState>();
        stream->audio = true;
        AVDictionary* options = nullptr;
        if (!openInput(audioInputFormatName(), audioInputName(config.microphoneDeviceId), AVMEDIA_TYPE_AUDIO, stream.get(), &options, error)) {
            av_dict_free(&options);
            return false;
        }
        av_dict_free(&options);

        const AVCodec* encoder = nullptr;
        if (config.audioCodec == AudioCodecPreset::Opus) {
            encoder = avcodec_find_encoder_by_name("libopus");
            if (!encoder) {
                encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
            }
        } else {
            encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        }
        if (!encoder) {
            if (error) {
                *error = QStringLiteral("No requested audio encoder found.");
            }
            return false;
        }

        stream->encoder = avcodec_alloc_context3(encoder);
        stream->encoder->codec_type = AVMEDIA_TYPE_AUDIO;
        stream->encoder->sample_rate = 48000;
        av_channel_layout_default(&stream->encoder->ch_layout, 2);
        stream->encoder->bit_rate = static_cast<int64_t>(config.audioTargetBitrateKbps) * 1000;
        stream->encoder->sample_fmt = encoder->sample_fmts ? encoder->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        stream->encoder->time_base = AVRational{1, stream->encoder->sample_rate};

        int rc = avcodec_open2(stream->encoder, encoder, nullptr);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to open audio encoder: %1").arg(avError(rc));
            }
            return false;
        }
        return addOutputStream(std::move(stream), error);
    }

    bool openInput(const char* formatName,
                   const QString& deviceName,
                   AVMediaType mediaType,
                   StreamState* stream,
                   AVDictionary** options,
                   QString* error) {
        const AVInputFormat* format = av_find_input_format(formatName);
        if (!format) {
            if (error) {
                *error = QStringLiteral("libavdevice input format '%1' is unavailable.").arg(formatName);
            }
            return false;
        }

        const QByteArray nameUtf8 = deviceName.toUtf8();
        int rc = avformat_open_input(&stream->inputFormat, nameUtf8.constData(), format, options);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to open capture device '%1': %2").arg(deviceName, avError(rc));
            }
            return false;
        }

        rc = avformat_find_stream_info(stream->inputFormat, nullptr);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to inspect capture device '%1': %2").arg(deviceName, avError(rc));
            }
            return false;
        }

        rc = av_find_best_stream(stream->inputFormat, mediaType, -1, -1, nullptr, 0);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Capture device '%1' has no usable stream: %2").arg(deviceName, avError(rc));
            }
            return false;
        }
        stream->inputStreamIndex = rc;
        stream->inputStream = stream->inputFormat->streams[stream->inputStreamIndex];

        const AVCodec* decoder = avcodec_find_decoder(stream->inputStream->codecpar->codec_id);
        if (!decoder) {
            if (error) {
                *error = QStringLiteral("No decoder found for capture stream.");
            }
            return false;
        }
        stream->decoder = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(stream->decoder, stream->inputStream->codecpar);
        rc = avcodec_open2(stream->decoder, decoder, nullptr);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to open capture decoder: %1").arg(avError(rc));
            }
            return false;
        }
        return true;
    }

    bool addOutputStream(std::unique_ptr<StreamState> stream, QString* error) {
        stream->outputStream = avformat_new_stream(outputFormat, nullptr);
        if (!stream->outputStream) {
            if (error) {
                *error = QStringLiteral("Failed to create MPEG-TS output stream.");
            }
            return false;
        }
        stream->outputStream->time_base = stream->encoder->time_base;
        const int rc = avcodec_parameters_from_context(stream->outputStream->codecpar, stream->encoder);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to copy encoder parameters: %1").arg(avError(rc));
            }
            return false;
        }
        streams.push_back(std::move(stream));
        return true;
    }

    void ensureCaptureEpoch() {
        std::int64_t expected = -1;
        captureEpochUs.compare_exchange_strong(expected, nowSteadyUs(), std::memory_order_acq_rel);
    }

    bool readObject(int packetsPerObject, M2tsObject* object, std::atomic<bool>& running, QString* error) {
        const int targetBytes = std::max(1, packetsPerObject) * 188;

        // An object must not cross a group boundary; the boundary becomes the
        // first byte of the next object (which starts a new group). Work in the
        // absolute byte space tracked by muxedConsumed.
        const std::uint64_t windowStart = muxedConsumed;
        std::uint64_t target = static_cast<std::uint64_t>(targetBytes);
        bool cutAtBoundary = false;

        const auto recomputeTarget = [&]() {
            // Drop boundaries at or before the window start (already consumed or
            // coincident with this object's first byte).
            while (!groupBoundaries.empty() && groupBoundaries.front() <= windowStart) {
                groupBoundaries.pop_front();
            }
            if (!groupBoundaries.empty()) {
                const std::uint64_t boundaryOffset = groupBoundaries.front() - windowStart;
                if (boundaryOffset > 0 && boundaryOffset < target) {
                    target = boundaryOffset;
                    cutAtBoundary = true;
                }
            }
        };
        recomputeTarget();

        while (running.load(std::memory_order_acquire) &&
               static_cast<std::uint64_t>(muxedBytes.size()) < target) {
            if (!pumpOnce(error)) {
                return false;
            }
            // pumpOnce may have appended new keyframe boundaries; a closer one
            // can tighten the target for this object.
            recomputeTarget();
        }

        const std::uint64_t available = static_cast<std::uint64_t>(muxedBytes.size());
        const std::uint64_t take = std::min(available, target);
        const int alignedBytes = static_cast<int>((take / 188) * 188);
        if (alignedBytes <= 0) {
            return false;
        }

        object->payload = muxedBytes.left(alignedBytes);
        muxedBytes.remove(0, alignedBytes);
        muxedConsumed += static_cast<std::uint64_t>(alignedBytes);

        // A new group begins when this object starts exactly on a recorded
        // boundary, or for the very first object overall.
        bool startGroup = (nextObjectId == 0);
        if (!startGroup && pendingGroupBoundary != 0 && windowStart == pendingGroupBoundary) {
            startGroup = true;
        }
        if (startGroup) {
            if (nextObjectId != 0) {
                ++nextGroupId;
            }
            object->groupId = nextGroupId;
            object->objectId = 0;
            object->startsGroup = true;
            nextObjectIdInGroup = 1;
        } else {
            object->groupId = nextGroupId;
            object->objectId = nextObjectIdInGroup++;
            object->startsGroup = false;
        }
        // If we cut this object at a boundary, that boundary is where the NEXT
        // object (and the next group) begins.
        if (cutAtBoundary && !groupBoundaries.empty()) {
            pendingGroupBoundary = groupBoundaries.front();
        }
        ++nextObjectId;
        return true;
    }

    void pumpUntilInitData() {
        QString ignored;
        for (int i = 0; i < 128 && initDataBytes.isEmpty(); ++i) {
            if (!pumpOnce(&ignored)) {
                break;
            }
            extractInitData(muxedBytes, &initDataBytes, &pmtPidValue, &pcrPidValue);
        }
    }

    bool pumpOnce(QString* error) {
        for (auto& stream : streams) {
            PacketPtr packet = makePacket();
            int rc = av_read_frame(stream->inputFormat, packet.get());
            if (rc < 0) {
                if (error) {
                    *error = QStringLiteral("Failed reading capture packet: %1").arg(avError(rc));
                }
                return false;
            }
            if (packet->stream_index != stream->inputStreamIndex) {
                continue;
            }
            rc = avcodec_send_packet(stream->decoder, packet.get());
            if (rc < 0) {
                // A corrupt input frame (e.g. transient MJPEG garbage at camera
                // startup) is not fatal: skip this packet and keep capturing,
                // mirroring ffmpeg's tolerance. Other errors still abort.
                if (rc == AVERROR_INVALIDDATA || rc == AVERROR(EINVAL)) {
                    continue;
                }
                if (error) {
                    *error = QStringLiteral("Failed sending packet to decoder: %1").arg(avError(rc));
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
                    // Skip a corrupt frame rather than tearing down the session.
                    if (rc == AVERROR_INVALIDDATA || rc == AVERROR(EINVAL)) {
                        break;
                    }
                    if (error) {
                        *error = QStringLiteral("Failed decoding capture frame: %1").arg(avError(rc));
                    }
                    return false;
                }
                if (!encodeFrame(stream.get(), frame.get(), error)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool sendEncoderFrame(StreamState* stream, AVFrame* frame, const char* streamKind, QString* error) {
        int rc = avcodec_send_frame(stream->encoder, frame);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed sending %1 frame to encoder: %2").arg(QString::fromLatin1(streamKind), avError(rc));
            }
            return false;
        }
        while (rc >= 0) {
            PacketPtr packet = makePacket();
            rc = avcodec_receive_packet(stream->encoder, packet.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
                break;
            }
            if (rc < 0) {
                if (error) {
                    *error = QStringLiteral("Failed receiving %1 encoded packet: %2").arg(QString::fromLatin1(streamKind), avError(rc));
                }
                return false;
            }
            av_packet_rescale_ts(packet.get(), stream->encoder->time_base, stream->outputStream->time_base);
            packet->stream_index = stream->outputStream->index;
            // Use the encoder's authoritative keyframe flag to mark MOQT group
            // boundaries. Flush the interleaver first so all previously queued
            // packets land in muxedBytes; the byte offset is then the exact start
            // of this keyframe's TS packets (group-aligned random access).
            const bool isVideoKey = stream->video && (packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (isVideoKey) {
                av_interleaved_write_frame(outputFormat, nullptr);
                const std::uint64_t offset = muxedConsumed + static_cast<std::uint64_t>(muxedBytes.size());
                if (!sawVideoKeyframe) {
                    // First IDR: group 0 starts at byte 0 and contains it; no push.
                    sawVideoKeyframe = true;
                    std::fprintf(stderr, "[moqxr][diag] keyframe groups: first IDR at offset=%llu\n",
                                 (unsigned long long)offset);
                } else {
                    groupBoundaries.push_back(offset);
                    ++rapBoundaryCount;
                    if (rapBoundaryCount % 30 == 1) {
                        std::fprintf(stderr,
                                     "[moqxr][diag] keyframe boundaries=%llu latest offset=%llu\n",
                                     (unsigned long long)rapBoundaryCount,
                                     (unsigned long long)offset);
                    }
                }
            }
            rc = av_interleaved_write_frame(outputFormat, packet.get());
            if (rc < 0) {
                if (error) {
                    *error = QStringLiteral("Failed writing MPEG-TS packet: %1").arg(avError(rc));
                }
                return false;
            }
        }
        return true;
    }

    void emitVideoPreview(StreamState* stream, AVFrame* frame) {
        if (!videoFrameCallback || frame->width <= 0 || frame->height <= 0) {
            return;
        }
        QImage image(frame->width, frame->height, QImage::Format_ARGB32);
        uint8_t* dstData[4] = {image.bits(), nullptr, nullptr, nullptr};
        int dstLinesize[4] = {image.bytesPerLine(), 0, 0, 0};
        stream->previewSws = sws_getCachedContext(stream->previewSws,
                                                  frame->width,
                                                  frame->height,
                                                  dejpegFormat(static_cast<AVPixelFormat>(frame->format)),
                                                  frame->width,
                                                  frame->height,
                                                  AV_PIX_FMT_BGRA,
                                                  SWS_BILINEAR,
                                                  nullptr,
                                                  nullptr,
                                                  nullptr);
        if (!stream->previewSws) {
            return;
        }
        applyScalerRange(stream->previewSws, static_cast<AVPixelFormat>(frame->format), /*dstRangeFull=*/true);
        sws_scale(stream->previewSws, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
        videoFrameCallback(image.copy());
    }

    static double previewSampleValue(AVSampleFormat format, const uint8_t* sample) {
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

    void emitAudioPreview(AVFrame* frame) {
        if (!audioLevelsCallback) {
            return;
        }
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
                const double value = previewSampleValue(format, ptr);
                sums[outChannel] += value * value;
            }
        }

        const double left = std::sqrt(sums[0] / samples);
        const double right = std::sqrt(sums[1] / samples);
        audioLevelsCallback(std::clamp(left, 0.0, 1.0), std::clamp(right, 0.0, 1.0));
    }

    bool encodeAudioFrame(StreamState* stream, AVFrame* inputFrame, QString* error) {
        emitAudioPreview(inputFrame);

        AVChannelLayout inputLayout = {};
        if (inputFrame->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&inputLayout, &inputFrame->ch_layout);
        } else {
            av_channel_layout_default(&inputLayout, inputFrame->ch_layout.nb_channels > 0 ? inputFrame->ch_layout.nb_channels : 2);
        }

        int rc = swr_alloc_set_opts2(&stream->swr,
                                     &stream->encoder->ch_layout,
                                     stream->encoder->sample_fmt,
                                     stream->encoder->sample_rate,
                                     &inputLayout,
                                     static_cast<AVSampleFormat>(inputFrame->format),
                                     inputFrame->sample_rate,
                                     0,
                                     nullptr);
        av_channel_layout_uninit(&inputLayout);
        if (rc < 0 || !stream->swr || swr_init(stream->swr) < 0) {
            if (error) {
                *error = QStringLiteral("Failed creating audio resampler.");
            }
            return false;
        }

        const int maxSamples = static_cast<int>(av_rescale_rnd(swr_get_delay(stream->swr, inputFrame->sample_rate) + inputFrame->nb_samples,
                                                               stream->encoder->sample_rate,
                                                               inputFrame->sample_rate,
                                                               AV_ROUND_UP));
        FramePtr resampled = makeFrame();
        resampled->format = stream->encoder->sample_fmt;
        resampled->sample_rate = stream->encoder->sample_rate;
        av_channel_layout_copy(&resampled->ch_layout, &stream->encoder->ch_layout);
        resampled->nb_samples = maxSamples;
        rc = av_frame_get_buffer(resampled.get(), 0);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed allocating audio frame: %1").arg(avError(rc));
            }
            return false;
        }

        const int convertedSamples = swr_convert(stream->swr,
                                                 resampled->extended_data,
                                                 maxSamples,
                                                 const_cast<const uint8_t**>(inputFrame->extended_data),
                                                 inputFrame->nb_samples);
        if (convertedSamples < 0) {
            if (error) {
                *error = QStringLiteral("Failed resampling audio frame: %1").arg(avError(convertedSamples));
            }
            return false;
        }
        if (convertedSamples == 0) {
            return true;
        }

        if (!stream->audioFifo) {
            stream->audioFifo = av_audio_fifo_alloc(stream->encoder->sample_fmt,
                                                    stream->encoder->ch_layout.nb_channels,
                                                    std::max(convertedSamples, stream->encoder->frame_size));
            if (!stream->audioFifo) {
                if (error) {
                    *error = QStringLiteral("Failed allocating audio FIFO.");
                }
                return false;
            }
        }

        rc = av_audio_fifo_realloc(stream->audioFifo, av_audio_fifo_size(stream->audioFifo) + convertedSamples);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed growing audio FIFO: %1").arg(avError(rc));
            }
            return false;
        }
        rc = av_audio_fifo_write(stream->audioFifo, reinterpret_cast<void**>(resampled->extended_data), convertedSamples);
        if (rc < convertedSamples) {
            if (error) {
                *error = QStringLiteral("Failed buffering audio samples.");
            }
            return false;
        }

        while (av_audio_fifo_size(stream->audioFifo) > 0) {
            const int targetSamples = stream->encoder->frame_size > 0
                ? stream->encoder->frame_size
                : av_audio_fifo_size(stream->audioFifo);
            if (av_audio_fifo_size(stream->audioFifo) < targetSamples) {
                break;
            }

            FramePtr frame = makeFrame();
            frame->format = stream->encoder->sample_fmt;
            frame->sample_rate = stream->encoder->sample_rate;
            av_channel_layout_copy(&frame->ch_layout, &stream->encoder->ch_layout);
            frame->nb_samples = targetSamples;
            rc = av_frame_get_buffer(frame.get(), 0);
            if (rc < 0) {
                if (error) {
                    *error = QStringLiteral("Failed allocating audio frame: %1").arg(avError(rc));
                }
                return false;
            }
            rc = av_audio_fifo_read(stream->audioFifo, reinterpret_cast<void**>(frame->extended_data), targetSamples);
            if (rc < targetSamples) {
                if (error) {
                    *error = QStringLiteral("Failed reading buffered audio samples.");
                }
                return false;
            }
            // Anchor audio to the shared capture epoch on its first frame, then
            // advance sample-accurately. This keeps audio sample-exact while
            // starting from the same zero as video so A/V stay in sync.
            ensureCaptureEpoch();
            if (!stream->audioAnchored) {
                const std::int64_t epoch = captureEpochUs.load(std::memory_order_acquire);
                const std::int64_t startUs = (epoch >= 0) ? (nowSteadyUs() - epoch) : 0;
                stream->nextAudioPts = av_rescale(startUs, stream->encoder->sample_rate, 1000000);
                stream->audioAnchored = true;
            }
            frame->pts = stream->nextAudioPts;
            stream->nextAudioPts += frame->nb_samples;
            if (!sendEncoderFrame(stream, frame.get(), "audio", error)) {
                return false;
            }
        }
        return true;
    }

    bool encodeFrame(StreamState* stream, AVFrame* inputFrame, QString* error) {
        if (stream->audio) {
            return encodeAudioFrame(stream, inputFrame, error);
        }

        emitVideoPreview(stream, inputFrame);

        FramePtr frame = makeFrame();
        frame->format = stream->encoder->pix_fmt;
        frame->width = stream->encoder->width;
        frame->height = stream->encoder->height;
        int rc = av_frame_get_buffer(frame.get(), 32);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed allocating video frame: %1").arg(avError(rc));
            }
            return false;
        }
        stream->sws = sws_getCachedContext(stream->sws,
                                           inputFrame->width,
                                           inputFrame->height,
                                           dejpegFormat(static_cast<AVPixelFormat>(inputFrame->format)),
                                           frame->width,
                                           frame->height,
                                           static_cast<AVPixelFormat>(frame->format),
                                           SWS_BILINEAR,
                                           nullptr,
                                           nullptr,
                                           nullptr);
        if (!stream->sws) {
            if (error) {
                *error = QStringLiteral("Failed creating video scaler.");
            }
            return false;
        }
        // Source MJPEG frames are full-range yuvj*; the H.264/MPEG-TS encode
        // target is limited-range YUV420P, so convert ranges explicitly.
        applyScalerRange(stream->sws, static_cast<AVPixelFormat>(inputFrame->format), /*dstRangeFull=*/false);
        sws_scale(stream->sws, inputFrame->data, inputFrame->linesize, 0, inputFrame->height, frame->data, frame->linesize);
        // Stamp PTS from a single steady-clock capture epoch shared with audio,
        // so both streams (and the mpegts PCR) are on one timeline. The wall
        // delta also gives libx264 real, increasing inter-frame timing so rate
        // control works (the old raw best_effort_timestamp flooded the encoder).
        ensureCaptureEpoch();
        const std::int64_t mediaUs = nowSteadyUs() - captureEpochUs.load(std::memory_order_acquire);
        frame->pts = av_rescale_q(mediaUs, AVRational{1, 1000000}, stream->encoder->time_base);
        return sendEncoderFrame(stream, frame.get(), "video", error);
    }
#else
    bool open(QString* error) {
        if (error) {
            *error = QStringLiteral("This build does not include libavdevice capture support.");
        }
        return false;
    }

    bool readObject(int, M2tsObject*, std::atomic<bool>&, QString* error) {
        if (error) {
            *error = QStringLiteral("This build does not include libavdevice capture support.");
        }
        return false;
    }

    void setPreviewCallbacks(std::function<void(const QImage&)>, std::function<void(double, double)>) {}
#endif
};

#ifdef Q_OS_DARWIN
QList<CaptureDevice> macEnumerateVideoInputs();
QList<CaptureDevice> macEnumerateAudioInputs();
#endif

QList<CaptureDevice> LibavCaptureSource::enumerateVideoInputs() {
#ifdef Q_OS_DARWIN
    return macEnumerateVideoInputs();
#endif
    QList<CaptureDevice> devices;
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    avdevice_register_all();
    const AVInputFormat* format = av_find_input_format(videoInputFormatName());
    if (!format) {
        return devices;
    }
    AVDeviceInfoList* list = nullptr;
    if (avdevice_list_input_sources(format, nullptr, nullptr, &list) < 0 || !list) {
        return devices;
    }
    QSet<QString> claimedNodes;  // nodes already folded into a camera group
    for (int i = 0; i < list->nb_devices; ++i) {
        AVDeviceInfo* info = list->devices[i];
        if (!info) continue;
        if (!info->device_name || !*info->device_name) continue;
        const QString deviceName = QString::fromUtf8(info->device_name);

#if defined(Q_OS_LINUX)
        if (claimedNodes.contains(deviceName)) {
            continue;  // already represented by an earlier camera group
        }
        // Group all capture nodes for this physical camera; keep only those
        // that actually report capture modes (drops metadata-only nodes).
        const std::vector<std::string> group =
            groupNodesForCamera(deviceName.toStdString());
        QStringList capable;
        for (const std::string& n : group) {
            const QString qn = QString::fromStdString(n);
            claimedNodes.insert(qn);
            if (!queryModes(n).empty()) {
                capable.append(qn);
            }
        }
        if (capable.isEmpty()) {
            continue;  // no usable capture node in this group
        }
        capable.sort();
        CaptureDevice d;
        d.id = capable.first();                  // stable anchor for QSettings
        d.description = QString::fromUtf8(info->device_description
                                          ? info->device_description : info->device_name);
        d.candidateNodes = capable;
        devices.append(d);
#else
        bool isVideo = false;
        for (int k = 0; k < info->nb_media_types; ++k) {
            if (info->media_types[k] == AVMEDIA_TYPE_VIDEO) { isVideo = true; break; }
        }
        if (info->nb_media_types == 0) {
            isVideo = true;
        }
        if (!isVideo) continue;
        CaptureDevice d;
        d.id = deviceName;
        d.description = QString::fromUtf8(info->device_description
                                          ? info->device_description : info->device_name);
        d.candidateNodes = QStringList{deviceName};
        devices.append(d);
#endif
    }
    avdevice_free_list_devices(&list);
#endif
    return devices;
}

QList<CaptureDevice> LibavCaptureSource::enumerateAudioInputs() {
#ifdef Q_OS_DARWIN
    return macEnumerateAudioInputs();
#endif
    QList<CaptureDevice> devices;
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    avdevice_register_all();
    const AVInputFormat* format = av_find_input_format(audioInputFormatName());
    if (!format) {
        return devices;
    }
    AVDeviceInfoList* list = nullptr;
    if (avdevice_list_input_sources(format, nullptr, nullptr, &list) < 0 || !list) {
        return devices;
    }
    for (int i = 0; i < list->nb_devices; ++i) {
        AVDeviceInfo* info = list->devices[i];
        if (!info) continue;
        if (!info->device_name || !*info->device_name) continue;
        bool isAudio = false;
        for (int k = 0; k < info->nb_media_types; ++k) {
            if (info->media_types[k] == AVMEDIA_TYPE_AUDIO) { isAudio = true; break; }
        }
        if (info->nb_media_types == 0) {
            isAudio = true;
        }
        if (!isAudio) continue;
        CaptureDevice d;
        d.id = QString::fromUtf8(info->device_name ? info->device_name : "");
        d.description = QString::fromUtf8(info->device_description ? info->device_description : info->device_name);
        devices.append(d);
    }
    avdevice_free_list_devices(&list);
#endif
    return devices;
}

LibavCaptureSource::LibavCaptureSource(PublishConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}

LibavCaptureSource::~LibavCaptureSource() = default;

bool LibavCaptureSource::open(QString* error) {
    return m_impl->open(error);
}

bool LibavCaptureSource::readObject(int packetsPerObject, M2tsObject* object, std::atomic<bool>& running, QString* error) {
    return m_impl->readObject(packetsPerObject, object, running, error);
}

void LibavCaptureSource::setPreviewCallbacks(std::function<void(const QImage&)> videoFrameCallback,
                                             std::function<void(double, double)> audioLevelsCallback) {
    m_impl->setPreviewCallbacks(std::move(videoFrameCallback), std::move(audioLevelsCallback));
}

int LibavCaptureSource::packetSize() const {
    return 188;
}

int LibavCaptureSource::programNumber() const {
    return 1;
}

int LibavCaptureSource::pmtPid() const {
    return m_impl->pmtPidValue;
}

int LibavCaptureSource::pcrPid() const {
    return m_impl->pcrPidValue;
}

QByteArray LibavCaptureSource::initData() const {
    return m_impl->initDataBytes;
}

bool LibavCaptureSource::randomAccessActive() const {
    // We can only honestly claim every group begins at a RAP once we are
    // grouping by keyframe, which requires a video stream and at least one
    // observed keyframe.
    return m_impl->hasVideoStream && m_impl->sawVideoKeyframe;
}

} // namespace moq2ts
