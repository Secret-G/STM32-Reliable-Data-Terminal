#ifndef DEVICE_PROTOCOL_H
#define DEVICE_PROTOCOL_H

#include "main.h"

/*
 * 设备应用层协议V1
 *
 * 线上的字节顺序固定为：
 * 帧头 | 版本 | 消息类型 | 设备ID | 序号 | 时间戳 | 数据长度 | 数据 | CRC16
 *
 * 多字节整数统一按大端序编码，帧头在线上固定为AA 55。
 * 编码函数逐字段写入发送缓冲区，不直接发送C结构体内存，避免填充和大小端差异。
 */
#define DEVICE_PROTOCOL_MAGIC             0xAA55U
#define DEVICE_PROTOCOL_VERSION           0x01U
#define DEVICE_PROTOCOL_MAX_PAYLOAD_SIZE  64U

#define DEVICE_PROTOCOL_OFFSET_MAGIC       0U
#define DEVICE_PROTOCOL_OFFSET_VERSION     2U
#define DEVICE_PROTOCOL_OFFSET_TYPE        3U
#define DEVICE_PROTOCOL_OFFSET_DEVICE_ID   4U
#define DEVICE_PROTOCOL_OFFSET_SEQUENCE    8U
#define DEVICE_PROTOCOL_OFFSET_TIMESTAMP   12U
#define DEVICE_PROTOCOL_OFFSET_LENGTH      16U
#define DEVICE_PROTOCOL_OFFSET_PAYLOAD     18U

/* 不含可变payload和CRC的固定字段长度。 */
#define DEVICE_PROTOCOL_FIXED_SIZE        18U
#define DEVICE_PROTOCOL_CRC_SIZE          2U
#define DEVICE_PROTOCOL_MAX_FRAME_SIZE    (DEVICE_PROTOCOL_FIXED_SIZE + \
                                           DEVICE_PROTOCOL_MAX_PAYLOAD_SIZE + \
                                           DEVICE_PROTOCOL_CRC_SIZE)

typedef enum
{
    DEVICE_MESSAGE_REALTIME_DATA = 0x01U, /* 实时采集数据 */
    DEVICE_MESSAGE_HISTORY_DATA  = 0x02U, /* 断网期间保存的历史数据 */
    DEVICE_MESSAGE_HEARTBEAT     = 0x03U, /* 设备在线心跳 */
    DEVICE_MESSAGE_COMMAND       = 0x10U, /* PC端下发命令 */
    DEVICE_MESSAGE_OTA_START     = 0x20U, /* 开始一次Wi-Fi OTA会话 */
    DEVICE_MESSAGE_OTA_DATA      = 0x21U, /* 固件数据；外层sequence即固件包序号 */
    DEVICE_MESSAGE_OTA_END       = 0x22U, /* 固件发送完成并请求整包校验 */
    DEVICE_MESSAGE_ACK           = 0x80U, /* 空Payload；设备ID和序号对应被确认的数据帧 */
    DEVICE_MESSAGE_OTA_RESPONSE  = 0xA0U  /* OTA命令的ACK/NACK统一响应 */
} DeviceProtocol_MessageType_t;

typedef enum
{
    DEVICE_PROTOCOL_OK = 0,
    DEVICE_PROTOCOL_ERROR_NULL,
    DEVICE_PROTOCOL_ERROR_PAYLOAD_LENGTH,
    DEVICE_PROTOCOL_ERROR_BUFFER_SIZE
} DeviceProtocol_Result_t;


typedef struct
{
    uint16_t magic;          /* buf[0~1]：帧头，2字节，依次为AA、55 */
    uint8_t version;         /* buf[2]：协议版本，1字节，当前为01 */
    uint8_t message_type;    /* buf[3]：消息类型，1字节，如01=实时数据、80=ACK */
    uint32_t device_id;      /* buf[4~7]：设备ID，4字节 */
    uint32_t sequence;       /* buf[8~11]：帧序号，4字节；ACK回显被确认帧的序号 */
    uint32_t timestamp;      /* buf[12~15]：时间戳，4字节；当前发送RTOS tick值 */
    uint16_t payload_length; /* buf[16~17]：正文长度N，2字节；不含帧头等固定字段和CRC */
    uint8_t payload[DEVICE_PROTOCOL_MAX_PAYLOAD_SIZE]; /* buf[18~17+N]：正文，只编码前N字节；N=0时不存在 */
    uint16_t crc16;          /* buf[18+N~19+N]：CRC，2字节；高字节在前，位置随N变化 */
} DeviceProtocol_Frame_t;

/* 应用协议拼帧缓存，不是USART接收RingBuffer；状态跨TCP分包保留。 */
typedef struct
{
    uint8_t data[DEVICE_PROTOCOL_MAX_FRAME_SIZE];
    uint16_t used;
} DeviceProtocol_Parser_t;

void DeviceProtocol_ParserReset(DeviceProtocol_Parser_t *parser);
/* 每次输入一个TCP正文字节；完整帧通过版本、长度和CRC检查后返回1。 */
uint8_t DeviceProtocol_ParserFeed(DeviceProtocol_Parser_t *parser, uint8_t byte,
                                 DeviceProtocol_Frame_t *frame);

/* 为一帧填入固定字段并清零其余内容，不执行编码或CRC计算。 */
void DeviceProtocol_FrameInit(DeviceProtocol_Frame_t *frame,
                              DeviceProtocol_MessageType_t message_type,
                              uint32_t device_id,
                              uint32_t sequence,
                              uint32_t timestamp);

/* CRC-16/CCITT-FALSE：Poly=0x1021，Init=0xFFFF，无反转，XorOut=0。 */
uint16_t DeviceProtocol_CalculateCrc16(const uint8_t *data, uint16_t length);

/*
 * 将字段逐个编码为大端网络字节流，并在末尾追加CRC16。
 * 成功时写回frame->crc16及encoded_length；不会直接操作TCP或USART。
 */
DeviceProtocol_Result_t DeviceProtocol_Encode(DeviceProtocol_Frame_t *frame,
                                              uint8_t *output,
                                              uint16_t output_capacity,
                                              uint16_t *encoded_length);

/* 将一个32位实时测试值按大端序写入4字节Payload。 */
DeviceProtocol_Result_t DeviceProtocol_SetRealtimeValue(DeviceProtocol_Frame_t *frame,
                                                        uint32_t value);

#endif
