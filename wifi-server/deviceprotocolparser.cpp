#include "deviceprotocolparser.h"

namespace {

quint16 readU16BE(const QByteArray &data, int offset)
{
    return (quint16(quint8(data.at(offset))) << 8U)
           | quint16(quint8(data.at(offset + 1)));
}

quint32 readU32BE(const QByteArray &data, int offset)
{
    return (quint32(quint8(data.at(offset))) << 24U)
           | (quint32(quint8(data.at(offset + 1))) << 16U)
           | (quint32(quint8(data.at(offset + 2))) << 8U)
           | quint32(quint8(data.at(offset + 3)));
}

} // namespace

QString DeviceFrame::messageTypeName() const
{
    switch (messageType) {
    case 0x01: return QStringLiteral("实时数据");
    case 0x02: return QStringLiteral("历史数据");
    case 0x03: return QStringLiteral("心跳");
    case 0x10: return QStringLiteral("命令");
    case 0x20: return QStringLiteral("OTA START");
    case 0x21: return QStringLiteral("OTA DATA");
    case 0x22: return QStringLiteral("OTA END");
    case 0x80: return QStringLiteral("ACK");
    case 0xA0: return QStringLiteral("OTA RESPONSE");
    default:   return QStringLiteral("未知 0x%1").arg(messageType, 2, 16, QLatin1Char('0')).toUpper();
    }
}

QList<DeviceFrame> DeviceProtocolParser::feed(const QByteArray &data)
{
    static const QByteArray magic = QByteArray::fromHex("AA55");
    QList<DeviceFrame> frames;
    m_buffer.append(data);

    while (true) {
        const qsizetype magicIndex = m_buffer.indexOf(magic);
        if (magicIndex < 0) {
            m_buffer = m_buffer.endsWith(char(0xAA)) ? QByteArray(1, char(0xAA)) : QByteArray();
            break;
        }

        if (magicIndex > 0)
            m_buffer.remove(0, magicIndex);

        if (m_buffer.size() < FixedSize)
            break;

        const quint16 payloadLength = readU16BE(m_buffer, 16);
        if (payloadLength > MaxPayloadSize) {
            m_buffer.remove(0, 1);
            continue;
        }

        const int frameLength = FixedSize + payloadLength + CrcSize;
        if (m_buffer.size() < frameLength)
            break;

        const QByteArray rawFrame = m_buffer.first(frameLength);
        m_buffer.remove(0, frameLength);

        const int crcPosition = FixedSize + payloadLength;
        DeviceFrame frame;
        frame.version = quint8(rawFrame.at(2));
        frame.messageType = quint8(rawFrame.at(3));
        frame.deviceId = readU32BE(rawFrame, 4);
        frame.sequence = readU32BE(rawFrame, 8);
        frame.timestamp = readU32BE(rawFrame, 12);
        frame.payload = rawFrame.mid(18, payloadLength);
        frame.receivedCrc = readU16BE(rawFrame, crcPosition);
        frame.calculatedCrc = calculateCrc16(rawFrame.first(crcPosition));
        frames.append(frame);
    }

    return frames;
}

void DeviceProtocolParser::clear()
{
    m_buffer.clear();
}

QByteArray DeviceProtocolParser::makeAck(const DeviceFrame &frame)
{
    // 仅确认支持的V1上行数据，CRC错误、未知类型和ACK都不回复。
    if (!frame.crcOk() || frame.version != 1 ||
        (frame.messageType != 0x01 && frame.messageType != 0x02 && frame.messageType != 0x03))
        return {};
    QByteArray ack = QByteArray::fromHex("AA550180");
    const auto appendU32 = [&ack](quint32 value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            ack.append(char((value >> shift) & 0xFFU));
    };
    appendU32(frame.deviceId);
    appendU32(frame.sequence);
    appendU32(frame.timestamp); // 回显原帧时间戳，不冒充PC时钟。
    ack.append(2, char(0)); // Payload长度为0，序号字段就是被确认序号。
    const quint16 crc = calculateCrc16(ack);
    ack.append(char(crc >> 8U));
    ack.append(char(crc & 0xFFU));
    return ack;
}

QByteArray DeviceProtocolParser::makeFrame(quint8 messageType, quint32 deviceId,
                                           quint32 sequence, quint32 timestamp,
                                           const QByteArray &payload)
{
    if (payload.size() > MaxPayloadSize)
        return {};

    QByteArray frame = QByteArray::fromHex("AA5501");
    frame.append(char(messageType));
    const auto appendU32 = [&frame](quint32 value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            frame.append(char((value >> shift) & 0xFFU));
    };
    appendU32(deviceId);
    appendU32(sequence);
    appendU32(timestamp);
    frame.append(char((payload.size() >> 8) & 0xFF));
    frame.append(char(payload.size() & 0xFF));
    frame.append(payload);
    const quint16 crc = calculateCrc16(frame);
    frame.append(char(crc >> 8U));
    frame.append(char(crc & 0xFFU));
    return frame;
}

quint16 DeviceProtocolParser::calculateCrc16(const QByteArray &data)
{
    quint16 crc = 0xFFFFU;
    for (const char byte : data) {
        crc ^= quint16(quint8(byte)) << 8U;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000U) ? quint16((crc << 1U) ^ 0x1021U) : quint16(crc << 1U);
    }
    return crc;
}
