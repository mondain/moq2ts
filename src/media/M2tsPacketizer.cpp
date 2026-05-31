#include "M2tsPacketizer.h"

#include <algorithm>

#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif

namespace moq2ts {

namespace {

int pidOf(const QByteArray& tsPacket) {
    if (tsPacket.size() < 4) {
        return -1;
    }
    return ((static_cast<unsigned char>(tsPacket[1]) & 0x1f) << 8) |
           static_cast<unsigned char>(tsPacket[2]);
}

bool payloadUnitStart(const QByteArray& tsPacket) {
    return tsPacket.size() >= 2 && (static_cast<unsigned char>(tsPacket[1]) & 0x40) != 0;
}

int payloadOffset(const QByteArray& tsPacket) {
    if (tsPacket.size() < 4) {
        return -1;
    }
    const int adaptationControl = (static_cast<unsigned char>(tsPacket[3]) >> 4) & 0x03;
    if (adaptationControl == 0 || adaptationControl == 2) {
        return -1;
    }
    int offset = 4;
    if (adaptationControl == 3) {
        if (tsPacket.size() < 5) {
            return -1;
        }
        offset += 1 + static_cast<unsigned char>(tsPacket[4]);
    }
    return offset < tsPacket.size() ? offset : -1;
}

bool extractPsiSection(const QByteArray& tsPacket, QByteArray* section) {
    if (!payloadUnitStart(tsPacket)) {
        return false;
    }
    const int offset = payloadOffset(tsPacket);
    if (offset < 0 || offset >= tsPacket.size()) {
        return false;
    }

    const int pointer = static_cast<unsigned char>(tsPacket[offset]);
    const int sectionStart = offset + 1 + pointer;
    if (sectionStart + 3 > tsPacket.size()) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(tsPacket[sectionStart + 1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(tsPacket[sectionStart + 2]);
    if (sectionLength < 5 || sectionStart + 3 + sectionLength > tsPacket.size()) {
        return false;
    }
    *section = tsPacket.mid(sectionStart, 3 + sectionLength);
    return true;
}

bool findPatProgram(const QByteArray& section, int requestedProgram, int* programNumber, int* pmtPid) {
    if (section.size() < 12 || static_cast<unsigned char>(section[0]) != 0x00) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(section[1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(section[2]);
    const int entriesEnd = 3 + sectionLength - 4;
    for (int offset = 8; offset + 4 <= entriesEnd; offset += 4) {
        const int program = (static_cast<unsigned char>(section[offset]) << 8) |
                            static_cast<unsigned char>(section[offset + 1]);
        const int pid = ((static_cast<unsigned char>(section[offset + 2]) & 0x1f) << 8) |
                        static_cast<unsigned char>(section[offset + 3]);
        if (program == 0) {
            continue;
        }
        if (requestedProgram == 0 || requestedProgram == program) {
            *programNumber = program;
            *pmtPid = pid;
            return true;
        }
    }
    return false;
}

bool parsePmt(const QByteArray& section, int* pcrPid, std::set<int>* elementaryPids) {
    if (section.size() < 16 || static_cast<unsigned char>(section[0]) != 0x02) {
        return false;
    }
    const int sectionLength = ((static_cast<unsigned char>(section[1]) & 0x0f) << 8) |
                              static_cast<unsigned char>(section[2]);
    const int sectionEnd = 3 + sectionLength - 4;
    if (sectionEnd > section.size() || sectionEnd < 12) {
        return false;
    }

    *pcrPid = ((static_cast<unsigned char>(section[8]) & 0x1f) << 8) |
              static_cast<unsigned char>(section[9]);
    const int programInfoLength = ((static_cast<unsigned char>(section[10]) & 0x0f) << 8) |
                                  static_cast<unsigned char>(section[11]);
    int offset = 12 + programInfoLength;
    while (offset + 5 <= sectionEnd) {
        const int elementaryPid = ((static_cast<unsigned char>(section[offset + 1]) & 0x1f) << 8) |
                                  static_cast<unsigned char>(section[offset + 2]);
        const int esInfoLength = ((static_cast<unsigned char>(section[offset + 3]) & 0x0f) << 8) |
                                 static_cast<unsigned char>(section[offset + 4]);
        elementaryPids->insert(elementaryPid);
        offset += 5 + esInfoLength;
    }
    return true;
}

} // namespace

M2tsPacketizer::M2tsPacketizer(QString sourcePath)
    : m_sourcePath(std::move(sourcePath)),
      m_file(m_sourcePath) {}

bool M2tsPacketizer::open(int requestedProgramNumber, QString* error) {
    m_requestedProgramNumber = requestedProgramNumber;
    if (!m_file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Failed to open M2TS source: %1").arg(m_file.errorString());
        }
        return false;
    }
    if (!detectPacketSize(error)) {
        return false;
    }
    if (!collectInitData(error)) {
        return false;
    }
    return m_file.seek(0);
}

bool M2tsPacketizer::detectPacketSize(QString* error) {
    const QByteArray probe = m_file.peek(192 * 4);
    if (probe.size() < 188) {
        if (error) {
            *error = QStringLiteral("Input is too small to contain a TS packet.");
        }
        return false;
    }

    const auto syncAt = [&probe](int offset) {
        return offset >= 0 && offset < probe.size() && static_cast<unsigned char>(probe[offset]) == 0x47;
    };

    if (syncAt(0) && (probe.size() < 188 * 2 || syncAt(188))) {
        m_packetSize = 188;
        return true;
    }
    if (syncAt(4) && (probe.size() < 192 * 2 || syncAt(196))) {
        m_packetSize = 192;
        return true;
    }

    if (error) {
        *error = QStringLiteral("Input is not packet-aligned TS/M2TS. Expected sync byte at offset 0 for 188-byte TS or offset 4 for 192-byte M2TS.");
    }
    return false;
}

bool M2tsPacketizer::packetHasSync(const QByteArray& packet) const {
    if (packet.size() != m_packetSize) {
        return false;
    }
    const int syncOffset = m_packetSize == 192 ? 4 : 0;
    return static_cast<unsigned char>(packet[syncOffset]) == 0x47;
}

QByteArray M2tsPacketizer::tsPacketView(const QByteArray& sourcePacket) const {
    if (m_packetSize == 192) {
        return sourcePacket.mid(4, 188);
    }
    return sourcePacket;
}

bool M2tsPacketizer::collectInitData(QString* error) {
    const qint64 originalPos = m_file.pos();
    if (!m_file.seek(0)) {
        if (error) {
            *error = QStringLiteral("Failed to seek M2TS source while collecting initData.");
        }
        return false;
    }

    QByteArray patPacket;
    QByteArray pmtPacket;
    std::set<int> elementaryPids;
    constexpr int maxPacketsToScan = 4096;
    for (int index = 0; index < maxPacketsToScan; ++index) {
        const QByteArray sourcePacket = m_file.read(m_packetSize);
        if (sourcePacket.size() != m_packetSize) {
            break;
        }
        if (!packetHasSync(sourcePacket)) {
            if (error) {
                *error = QStringLiteral("Invalid TS sync byte while collecting initData.");
            }
            return false;
        }

        const QByteArray tsPacket = tsPacketView(sourcePacket);
        const int pid = pidOf(tsPacket);
        if (pid == 0 && patPacket.isEmpty()) {
            QByteArray patSection;
            if (extractPsiSection(tsPacket, &patSection) &&
                findPatProgram(patSection, m_requestedProgramNumber, &m_programNumber, &m_pmtPid)) {
                patPacket = sourcePacket;
            }
        } else if (m_pmtPid >= 0 && pid == m_pmtPid && pmtPacket.isEmpty()) {
            QByteArray pmtSection;
            if (extractPsiSection(tsPacket, &pmtSection) && !pmtSection.isEmpty() &&
                parsePmt(pmtSection, &m_pcrPid, &elementaryPids)) {
                pmtPacket = sourcePacket;
            }
        }

        if (!patPacket.isEmpty() && !pmtPacket.isEmpty()) {
            break;
        }
    }

    if (!patPacket.isEmpty()) {
        m_initData += patPacket;
    }
    if (!pmtPacket.isEmpty()) {
        m_initData += pmtPacket;
    }

    if (m_initData.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Failed to collect PAT/PMT packets for catalog initData.");
        }
        return false;
    }

    m_selectedPids.clear();
    m_selectedPids.insert(0x0000);
    if (m_pmtPid >= 0) {
        m_selectedPids.insert(m_pmtPid);
    }
    if (m_pcrPid >= 0) {
        m_selectedPids.insert(m_pcrPid);
    }
    m_selectedPids.insert(elementaryPids.begin(), elementaryPids.end());

    if (m_requestedProgramNumber != 0 && m_programNumber != m_requestedProgramNumber) {
        if (error) {
            *error = QStringLiteral("Requested program %1 was not found in PAT.").arg(m_requestedProgramNumber);
        }
        return false;
    }
    if (m_selectedPids.size() <= 2) {
        if (error) {
            *error = QStringLiteral("Failed to parse selected program PMT elementary PIDs.");
        }
        return false;
    }

    return m_file.seek(originalPos);
}

bool M2tsPacketizer::readObject(int packetsPerObject, M2tsObject* object, QString* error) {
    if (object == nullptr || m_packetSize <= 0) {
        if (error) {
            *error = QStringLiteral("Packetizer is not open.");
        }
        return false;
    }

    const int packets = std::max(1, packetsPerObject);
    QByteArray payload;
    payload.reserve(packets * m_packetSize);

    for (int index = 0; index < packets; ++index) {
        const QByteArray packet = m_file.read(m_packetSize);
        if (packet.isEmpty()) {
            break;
        }
        if (packet.size() != m_packetSize) {
            if (error) {
                *error = QStringLiteral("Source ended on a partial TS/M2TS packet.");
            }
            return false;
        }
        if (!packetHasSync(packet)) {
            if (error) {
                *error = QStringLiteral("Invalid TS sync byte in source packet.");
            }
            return false;
        }
        const int pid = pidOf(tsPacketView(packet));
        if (m_selectedPids.find(pid) == m_selectedPids.end()) {
            --index;
            continue;
        }
        payload += packet;
    }

    if (payload.isEmpty()) {
        return false;
    }

    object->payload = std::move(payload);
    object->groupId = 0;
    object->objectId = m_nextObjectId++;
    return true;
}

int M2tsPacketizer::packetSize() const {
    return m_packetSize;
}

int M2tsPacketizer::programNumber() const {
    return m_programNumber;
}

int M2tsPacketizer::pmtPid() const {
    return m_pmtPid;
}

int M2tsPacketizer::pcrPid() const {
    return m_pcrPid;
}

QByteArray M2tsPacketizer::initData() const {
    return m_initData;
}

std::uint64_t M2tsPacketizer::objectsRead() const {
    return m_nextObjectId;
}

qint64 M2tsPacketizer::probeDurationMs(const QString& sourcePath) {
#ifdef MOQ2TS_HAVE_LIBAV_CAPTURE
    if (sourcePath.isEmpty()) {
        return 0;
    }
    AVFormatContext* ctx = nullptr;
    const QByteArray path = sourcePath.toUtf8();
    if (avformat_open_input(&ctx, path.constData(), nullptr, nullptr) != 0) {
        return 0;
    }
    qint64 durationMs = 0;
    if (avformat_find_stream_info(ctx, nullptr) >= 0 && ctx->duration > 0) {
        // AVFormatContext::duration is in AV_TIME_BASE units (microseconds).
        durationMs = static_cast<qint64>((ctx->duration + (AV_TIME_BASE / 2000)) / (AV_TIME_BASE / 1000));
    }
    avformat_close_input(&ctx);
    return durationMs;
#else
    Q_UNUSED(sourcePath);
    return 0;
#endif
}

} // namespace moq2ts
