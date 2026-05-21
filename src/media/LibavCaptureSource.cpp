#include "LibavCaptureSource.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

#include <QtGlobal>

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
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

QString avError(int code) {
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
#else
    Q_UNUSED(code);
    return QStringLiteral("libav capture support is not compiled in.");
#endif
}

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

int writePacket(void* opaque, const uint8_t* buffer, int bufferSize) {
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

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    struct StreamState {
        AVFormatContext* inputFormat = nullptr;
        AVCodecContext* decoder = nullptr;
        AVCodecContext* encoder = nullptr;
        AVStream* inputStream = nullptr;
        AVStream* outputStream = nullptr;
        SwsContext* sws = nullptr;
        SwrContext* swr = nullptr;
        int inputStreamIndex = -1;
        bool audio = false;
        bool video = false;
    };

    std::vector<std::unique_ptr<StreamState>> streams;
    AVFormatContext* outputFormat = nullptr;
    AVIOContext* outputIo = nullptr;
    unsigned char* outputIoBuffer = nullptr;
    bool headerWritten = false;

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
            if (stream->swr) {
                swr_free(&stream->swr);
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

        pumpUntilInitData();
        if (initDataBytes.isEmpty()) {
            if (error) {
                *error = QStringLiteral("MPEG-TS capture muxer did not emit PAT/PMT initData.");
            }
            return false;
        }
        return true;
    }

    bool addVideo(QString* error) {
        auto stream = std::make_unique<StreamState>();
        stream->video = true;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "framerate", QByteArray::number(config.videoFramerate).constData(), 0);
        av_dict_set(&options, "video_size", QStringLiteral("%1x%2").arg(config.videoWidth).arg(config.videoHeight).toUtf8().constData(), 0);
        if (!openInput(videoInputFormatName(), videoInputName(config.cameraDeviceId), AVMEDIA_TYPE_VIDEO, stream.get(), &options, error)) {
            av_dict_free(&options);
            return false;
        }
        av_dict_free(&options);

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
        stream->encoder->time_base = AVRational{1, config.videoFramerate};
        stream->encoder->framerate = AVRational{config.videoFramerate, 1};
        stream->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        stream->encoder->bit_rate = static_cast<int64_t>(config.videoTargetBitrateKbps) * 1000;
        av_opt_set(stream->encoder->priv_data, "profile", "baseline", 0);
        int rc = avcodec_open2(stream->encoder, encoder, nullptr);
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed to open H.264 encoder: %1").arg(avError(rc));
            }
            return false;
        }
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

    bool readObject(int packetsPerObject, M2tsObject* object, std::atomic<bool>& running, QString* error) {
        const int targetBytes = std::max(1, packetsPerObject) * 188;
        while (running.load(std::memory_order_acquire) && muxedBytes.size() < targetBytes) {
            if (!pumpOnce(error)) {
                return false;
            }
        }

        const int alignedBytes = (std::min<qsizetype>(muxedBytes.size(), targetBytes) / 188) * 188;
        if (alignedBytes <= 0) {
            return false;
        }

        object->payload = muxedBytes.left(alignedBytes);
        muxedBytes.remove(0, alignedBytes);
        object->groupId = 0;
        object->objectId = nextObjectId++;
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

    bool encodeFrame(StreamState* stream, AVFrame* inputFrame, QString* error) {
        FramePtr frame = makeFrame();
        if (stream->video) {
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
                                               static_cast<AVPixelFormat>(inputFrame->format),
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
            sws_scale(stream->sws, inputFrame->data, inputFrame->linesize, 0, inputFrame->height, frame->data, frame->linesize);
            frame->pts = inputFrame->best_effort_timestamp;
            if (frame->pts == AV_NOPTS_VALUE) {
                frame->pts = nextObjectId;
            }
        } else {
            frame->format = stream->encoder->sample_fmt;
            frame->sample_rate = stream->encoder->sample_rate;
            av_channel_layout_copy(&frame->ch_layout, &stream->encoder->ch_layout);
            frame->nb_samples = inputFrame->nb_samples;
            int rc = av_frame_get_buffer(frame.get(), 0);
            if (rc < 0) {
                if (error) {
                    *error = QStringLiteral("Failed allocating audio frame: %1").arg(avError(rc));
                }
                return false;
            }
            AVChannelLayout inputLayout;
            if (inputFrame->ch_layout.nb_channels > 0) {
                av_channel_layout_copy(&inputLayout, &inputFrame->ch_layout);
            } else {
                av_channel_layout_default(&inputLayout, inputFrame->ch_layout.nb_channels > 0 ? inputFrame->ch_layout.nb_channels : 2);
            }
            rc = swr_alloc_set_opts2(&stream->swr,
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
            swr_convert(stream->swr, frame->data, frame->nb_samples, const_cast<const uint8_t**>(inputFrame->extended_data), inputFrame->nb_samples);
            frame->pts = av_rescale_q(inputFrame->best_effort_timestamp, stream->inputStream->time_base, stream->encoder->time_base);
        }

        int rc = avcodec_send_frame(stream->encoder, frame.get());
        if (rc < 0) {
            if (error) {
                *error = QStringLiteral("Failed sending frame to encoder: %1").arg(avError(rc));
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
                    *error = QStringLiteral("Failed receiving encoded packet: %1").arg(avError(rc));
                }
                return false;
            }
            av_packet_rescale_ts(packet.get(), stream->encoder->time_base, stream->outputStream->time_base);
            packet->stream_index = stream->outputStream->index;
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
    for (int i = 0; i < list->nb_devices; ++i) {
        AVDeviceInfo* info = list->devices[i];
        if (!info) continue;
        bool isVideo = false;
        for (int k = 0; k < info->nb_media_types; ++k) {
            if (info->media_types[k] == AVMEDIA_TYPE_VIDEO) { isVideo = true; break; }
        }
        if (info->nb_media_types == 0) {
            isVideo = true;
        }
        if (!isVideo) continue;
        CaptureDevice d;
        d.id = QString::number(i);
        d.description = QString::fromUtf8(info->device_description ? info->device_description : info->device_name);
        devices.append(d);
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
        bool isAudio = false;
        for (int k = 0; k < info->nb_media_types; ++k) {
            if (info->media_types[k] == AVMEDIA_TYPE_AUDIO) { isAudio = true; break; }
        }
        if (info->nb_media_types == 0) {
            isAudio = true;
        }
        if (!isAudio) continue;
        CaptureDevice d;
        d.id = QString::number(i);
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

} // namespace moq2ts
