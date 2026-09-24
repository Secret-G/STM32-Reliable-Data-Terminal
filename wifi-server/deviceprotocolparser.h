#ifndef DEVICEPROTOCOLPARSER_H
#define DEVICEPROTOCOLPARSER_H

#include <QByteArray>
#include <QList>
#include <QString>

struct DeviceFrame
{
    quint8 version = 0;
    quint8 messageType = 0;
    quint32 deviceId = 0;
    quint32 sequence = 0;
    quint32 timestamp = 0;
    QByteArray payload;
    quint16 receivedCrc = 0;
    quint16 calculatedCrc = 0;

    bool crcOk() const { return receivedCrc == calculatedCrc; }
    QString messageTypeName() const;
};

class DeviceProtocolParser
{
public:
    QList<DeviceFrame> feed(const QByteArray &data);
    void clear();

    static quint16 calculateCrc16(const QByteArray &data);
    static QByteArray makeFrame(quint8 messageType, quint32 deviceId,
                                quint32 sequence, quint32 timestamp,
                                const QByteArray &payload);
    // ACK沿用原设备ID/序号，空Payload；ACK本身不再请求ACK。
    static QByteArray makeAck(const DeviceFrame &frame);

private:
    static constexpr int FixedSize = 18;
    static constexpr int CrcSize = 2;
    static constexpr int MaxPayloadSize = 64;

    QByteArray m_buffer;
};

#endif // DEVICEPROTOCOLPARSER_H
