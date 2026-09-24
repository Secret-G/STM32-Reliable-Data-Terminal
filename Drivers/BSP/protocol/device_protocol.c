#include "device_protocol.h"

#include <string.h>

static uint16_t DeviceProtocol_ReadU16BE(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint32_t DeviceProtocol_ReadU32BE(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | data[3];
}

void DeviceProtocol_ParserReset(DeviceProtocol_Parser_t *parser)
{
    if (parser != NULL) parser->used = 0U;
}

uint8_t DeviceProtocol_ParserFeed(DeviceProtocol_Parser_t *parser, uint8_t byte, DeviceProtocol_Frame_t *frame)
{
    uint16_t length;
    uint16_t total;
    uint16_t crc;
    DeviceProtocol_MessageType_t message_type;
    uint32_t device_id;
    uint32_t sequence;
    uint32_t timestamp;
    if ((parser == NULL) || (frame == NULL)) return 0U;

    parser->data[parser->used++] = byte;

    while (parser->used >= 2U)
    {
        /*判断帧头*/
        if (DeviceProtocol_ReadU16BE(parser->data) == DEVICE_PROTOCOL_MAGIC)
        {
            /*错误帧*/
            if (parser->used < DEVICE_PROTOCOL_FIXED_SIZE) return 0U;

            /*获取有效载荷长度*/
            length = DeviceProtocol_ReadU16BE(&parser->data[16]);

            /*判断设备版本和载荷长度*/
            if ((parser->data[2] == DEVICE_PROTOCOL_VERSION) &&  (length <= DEVICE_PROTOCOL_MAX_PAYLOAD_SIZE))
            {
                /*计算总长度：固定字段+可变payload+CRC */
                total = (uint16_t)(DEVICE_PROTOCOL_FIXED_SIZE + length + DEVICE_PROTOCOL_CRC_SIZE);

                /* 判断是否收齐一整帧：如果不够就继续等待下一个字节 */
                if (parser->used < total) return 0U;

                /* 计算CRC并校验 */
                crc = DeviceProtocol_ReadU16BE(&parser->data[total - 2U]);
                if (crc == DeviceProtocol_CalculateCrc16(parser->data, (uint16_t)(total - 2U)))
                {
                    /* 先按数组下标取出各字段，便于逐步调试、查看解析结果。 */
                    message_type = (DeviceProtocol_MessageType_t)parser->data[3]; /* [3]：消息类型 */
                    device_id = DeviceProtocol_ReadU32BE(&parser->data[4]);      /* [4~7]：设备ID */
                    sequence = DeviceProtocol_ReadU32BE(&parser->data[8]);       /* [8~11]：序号 */
                    timestamp = DeviceProtocol_ReadU32BE(&parser->data[12]);     /* [12~15]：时间戳 */

                    /* 再将读出的值传入初始化函数，不在参数中嵌套读取函数。 */
                    DeviceProtocol_FrameInit(frame, message_type, device_id, sequence, timestamp);
                    
                    /* 写入可变payload和长度，注意payload可能为0字节。 */
                    frame->payload_length = length;
                    memcpy(frame->payload, &parser->data[18], length);
                    /* 写入CRC */
                    frame->crc16 = crc;
                    
                    parser->used = (uint16_t)(parser->used - total);
                    memmove(parser->data, &parser->data[total], parser->used);
                    return 1U;
                }
            }
        }
        /* 非法帧逐字节重新寻找AA55，也保留末尾可能的半个帧头。 */
        parser->used--;
        memmove(parser->data, &parser->data[1], parser->used);
    }
    return 0U;
}

static void DeviceProtocol_WriteU16BE(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void DeviceProtocol_WriteU32BE(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

void DeviceProtocol_FrameInit(DeviceProtocol_Frame_t *frame,
                              DeviceProtocol_MessageType_t message_type,
                              uint32_t device_id,
                              uint32_t sequence,
                              uint32_t timestamp)
{
    if (frame == NULL)
    {
        return;
    }

    memset(frame, 0, sizeof(*frame));
    frame->magic = DEVICE_PROTOCOL_MAGIC;
    frame->version = DEVICE_PROTOCOL_VERSION;
    frame->message_type = (uint8_t)message_type;
    frame->device_id = device_id;
    frame->sequence = sequence;
    frame->timestamp = timestamp;
}

uint16_t DeviceProtocol_CalculateCrc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    uint16_t index;
    uint8_t bit;

    if ((data == NULL) && (length != 0U))
    {
        return 0U;
    }

    for (index = 0U; index < length; index++)
    {
        crc ^= (uint16_t)((uint16_t)data[index] << 8U);
        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            }
            else
            {
                crc = (uint16_t)(crc << 1U);
            }
        }
    }

    return crc;
}

DeviceProtocol_Result_t DeviceProtocol_Encode(DeviceProtocol_Frame_t *frame,
                                              uint8_t *output,
                                              uint16_t output_capacity,
                                              uint16_t *encoded_length)
{
    uint16_t frame_length;
    uint16_t crc_position;

    if ((frame == NULL) || (output == NULL) || (encoded_length == NULL))
    {
        return DEVICE_PROTOCOL_ERROR_NULL;
    }

    *encoded_length = 0U;

    if (frame->payload_length > DEVICE_PROTOCOL_MAX_PAYLOAD_SIZE)
    {
        return DEVICE_PROTOCOL_ERROR_PAYLOAD_LENGTH;
    }

    frame_length = (uint16_t)(DEVICE_PROTOCOL_FIXED_SIZE +
                              frame->payload_length +
                              DEVICE_PROTOCOL_CRC_SIZE);
    if (output_capacity < frame_length)
    {
        return DEVICE_PROTOCOL_ERROR_BUFFER_SIZE;
    }

    /* 明确按协议偏移逐字段写入，避免结构体对齐和CPU大小端产生影响。 */
    DeviceProtocol_WriteU16BE(&output[DEVICE_PROTOCOL_OFFSET_MAGIC], frame->magic);
    output[DEVICE_PROTOCOL_OFFSET_VERSION] = frame->version;
    output[DEVICE_PROTOCOL_OFFSET_TYPE] = frame->message_type;
    DeviceProtocol_WriteU32BE(&output[DEVICE_PROTOCOL_OFFSET_DEVICE_ID], frame->device_id);
    DeviceProtocol_WriteU32BE(&output[DEVICE_PROTOCOL_OFFSET_SEQUENCE], frame->sequence);
    DeviceProtocol_WriteU32BE(&output[DEVICE_PROTOCOL_OFFSET_TIMESTAMP], frame->timestamp);
    DeviceProtocol_WriteU16BE(&output[DEVICE_PROTOCOL_OFFSET_LENGTH], frame->payload_length);

    if (frame->payload_length > 0U)
    {
        memcpy(&output[DEVICE_PROTOCOL_OFFSET_PAYLOAD],frame->payload,frame->payload_length);
    }

    crc_position = (uint16_t)(DEVICE_PROTOCOL_OFFSET_PAYLOAD + frame->payload_length);
    frame->crc16 = DeviceProtocol_CalculateCrc16(output, crc_position);
    DeviceProtocol_WriteU16BE(&output[crc_position], frame->crc16);

    *encoded_length = frame_length;
    return DEVICE_PROTOCOL_OK;
}

DeviceProtocol_Result_t DeviceProtocol_SetRealtimeValue(DeviceProtocol_Frame_t *frame,
                                                        uint32_t value)
{
    if (frame == NULL)
    {
        return DEVICE_PROTOCOL_ERROR_NULL;
    }

    /* Payload字段同样明确使用大端序，不能直接复制uint32_t内存。 */
    DeviceProtocol_WriteU32BE(frame->payload, value);
    frame->payload_length = 4U;
    return DEVICE_PROTOCOL_OK;
}
