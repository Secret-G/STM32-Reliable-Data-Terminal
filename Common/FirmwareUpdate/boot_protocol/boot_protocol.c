#include "boot_protocol.h"
#include "boot_crc.h"

static uint8_t Boot_FrameSofValid(const uint8_t *frame)
{
    if (frame == NULL)
    {
        return 0U;
    }

    if ((frame[BOOT_FRAME_SOF_OFFSET] != BOOT_FRAME_SOF_BYTE0) ||
        (frame[BOOT_FRAME_SOF_OFFSET + 1U] != BOOT_FRAME_SOF_BYTE1))
    {
        return 0U;
    }

    return 1U;
}

uint16_t Boot_ParseCmd(uint8_t *frame)
{
    uint16_t cmd;

    if (frame == NULL)
    {
        return 0U;
    }

    cmd = ((uint16_t)frame[BOOT_FRAME_CMD_OFFSET]) |
          ((uint16_t)frame[BOOT_FRAME_CMD_OFFSET + 1U] << 8);
    return cmd;
}

uint16_t Boot_ParseLength(uint8_t *frame)
{
    uint16_t len;
    if (frame == NULL)
    {
        return 0U;
    }

    len = ((uint16_t)frame[BOOT_FRAME_LENGTH_OFFSET]) |
          ((uint16_t)frame[BOOT_FRAME_LENGTH_OFFSET + 1U] << 8);
    return len;
}

uint16_t Boot_ParseCRC(uint8_t *frame,uint16_t total_len)
{
    uint16_t crc;

    if ((frame == NULL) || (total_len < BOOT_FRAME_MIN_SIZE) 
                        || (total_len > BOOT_FRAME_MAX_SIZE))
    {
        return 0U;
    }
    

    crc = ((uint16_t)frame[total_len - 2U]) |
          ((uint16_t)frame[total_len - 1U] << 8);

    return crc;
}

uint16_t Boot_GetDataLength(uint16_t total_len)
{
    if ((total_len < BOOT_FRAME_MIN_SIZE) || (total_len > BOOT_FRAME_MAX_SIZE))
    {
        return 0U;
    }

    return total_len - BOOT_FRAME_FIXED_SIZE;
}

uint32_t Boot_ParseReserve(uint8_t *frame, uint16_t total_len)
{
    uint32_t reserve;
    uint16_t reserve_offset;
    
    if ((frame == NULL) || (total_len < BOOT_FRAME_MIN_SIZE) 
                        || (total_len > BOOT_FRAME_MAX_SIZE))
    {
        return 0U;
    }
    
    reserve_offset  = total_len - BOOT_FRAME_CRC_SIZE - BOOT_FRAME_RESERVE_SIZE;

    reserve = ((uint32_t)frame[reserve_offset]) |
              ((uint32_t)frame[reserve_offset + 1U] << 8) |
              ((uint32_t)frame[reserve_offset + 2U] << 16) |
              ((uint32_t)frame[reserve_offset + 3U] << 24);

    
    return reserve;
}

uint8_t Boot_VerifyFrameCRC(uint8_t *frame,uint16_t total_len)
{
    uint16_t received_crc;
    uint16_t calculated_crc;

    if ((frame == NULL) ||
        (Boot_FrameSofValid(frame) == 0U) ||
        (total_len < BOOT_FRAME_FIXED_SIZE) ||
        (total_len > BOOT_FRAME_MAX_SIZE))
    {
        return 0U;
    }

    /*上位机发送的crc*/
    received_crc = Boot_ParseCRC(frame, total_len);

    /*实际根据数据计算得到的crc*/
    calculated_crc = Boot_CRC16_Modbus(frame,total_len - BOOT_FRAME_CRC_SIZE);

    if (received_crc != calculated_crc)
    {
        return 0U;
    }

    return 1U;
}

Update_TargetTypeDef Boot_ParseStartTarget(uint8_t *frame)
{
    uint8_t start_target;

    if (frame == NULL)
    {
        return UPDATE_NONE;
    }

    start_target = frame[BOOT_FRAME_DATA_OFFSET + BOOT_START_TARGET_OFFSET];
    
    if(start_target == (uint8_t)UPDATE_APP_A)
    {
        return UPDATE_APP_A;
    }

    if(start_target == (uint8_t)UPDATE_APP_B)
    {
        return UPDATE_APP_B;
    }

    if (start_target == (uint8_t)UPDATE_AUTO)
    {
        return UPDATE_AUTO;
    }
    
    return UPDATE_NONE;
}

uint32_t Boot_ParseStartImageSize(uint8_t *frame)
{
    uint16_t offset;
    uint32_t image_size;

    if(frame == NULL)
    {
        return 0U;
    }

    offset = BOOT_FRAME_DATA_OFFSET + BOOT_START_IMAGE_SIZE_OFFSET;
    
    image_size = (uint32_t)frame[offset] | 
               ((uint32_t)frame[offset + 1] << 8) | 
               ((uint32_t)frame[offset + 2] << 16) | 
               ((uint32_t)frame[offset + 3] << 24);
    
    return image_size;
}

uint16_t Boot_ParseStartImageCRC(uint8_t *frame)
{
    uint16_t offset;
    uint16_t image_crc;

    if (frame == NULL)
    {
        return 0U;
    }
    
    offset = BOOT_FRAME_DATA_OFFSET + BOOT_START_IMAGE_CRC_OFFSET;
    
    image_crc = (uint16_t)frame[offset] | 
               ((uint16_t)frame[offset + 1] << 8);
    
    return image_crc;
}

uint8_t Boot_ParseStartFrame(uint8_t *frame,uint16_t received_len,Boot_StartInfoTypeDef *start_info)
{
    uint16_t cmd;
    uint16_t total_len;
    Update_TargetTypeDef target;

    if((frame == NULL) || (start_info == NULL))
    {
        return 0U;
    }

    /*START帧固定为19字节,received_len表示接收器实际收到的字节数。*/
    if (received_len != BOOT_START_FRAME_SIZE)
    {
        return 0U;
    }
    
    /*检查固定帧头。*/
    if (Boot_FrameSofValid(frame) == 0U)
    {
        return 0U;
    }

    /*检查是不是开始命令*/
    cmd = Boot_ParseCmd(frame);
    if(cmd != (uint16_t)CMD_START_UPDATE)
    {
        return 0U;
    }
    
    /*读取帧内部声明的总长度*/
    total_len = Boot_ParseLength(frame);

    /*START帧的长度字节必须是19，而且必须与实际收到的长度一致*/
     if((total_len != BOOT_START_FRAME_SIZE) || (total_len != received_len))
     {
        return 0U;
     }

    /*检查当前START帧自己的CRC。*/
    if (Boot_VerifyFrameCRC(frame,total_len) == 0U)
    {
        return 0U;
    }

    /*解析并检查目标A/B。*/
    target = Boot_ParseStartTarget(frame);

    if (target == UPDATE_NONE)
    {
        return 0U;
    }

    /*保存START帧解析结果。*/
    start_info->target = target;
    start_info->image_size = Boot_ParseStartImageSize(frame);
    start_info->image_crc = Boot_ParseStartImageCRC(frame);
    start_info->image_version = Boot_ParseReserve(frame, total_len);

    /*BIN文件长度不能为0*/
    if (start_info->image_size == 0U)
    {
        return 0U;
    }
    return 1;
}

uint8_t Boot_ParseDataFrame(uint8_t *frame,uint16_t received_len,Boot_DataInfoTypeDef *data_info)
{
    uint16_t cmd;
    uint16_t total_len;
    uint16_t data_len;

    /*检查输入和输出地址。*/
    if ((frame == NULL) || (data_info == NULL)){    return 0U;  }

    /*DATA帧实际长度必须在13～268字节之间。*/
    if ((received_len < BOOT_DATA_MIN_FRAME_SIZE) || (received_len > BOOT_DATA_MAX_FRAME_SIZE))
    {
        return 0U;
    }

    /*检查固定帧头。*/
    if (Boot_FrameSofValid(frame) == 0U){  return 0U;  }

    /*检查当前帧是不是DATA命令*/
    cmd = Boot_ParseCmd(frame);
    if (cmd != (uint16_t)CMD_DATA_PACKET){  return 0U;  }

    /*读取帧内部声明的总长度*/
    total_len = Boot_ParseLength(frame);

    /*帧内声明长度必须等于实际收到的长度*/
    if (total_len != received_len){ return 0U;  }

    /*根据总长度计算当前包的BIN数据长度。*/
    data_len = Boot_GetDataLength(total_len);

    /*BIN数据长度必须在1～256字节之间*/
    if ((data_len < BOOT_DATA_MIN_DATA_SIZE) || (data_len > BOOT_DATA_MAX_DATA_SIZE))
    {
        return 0U;
    }

    /*检查当前DATA帧自己的CRC。*/
    if (Boot_VerifyFrameCRC(frame, total_len) == 0U)
    {
        return 0U;
    }

    /*保存DATA帧解析结果*/
    data_info->data = &frame[BOOT_FRAME_DATA_OFFSET];
    data_info->data_len = data_len;
    data_info->sequence = Boot_ParseReserve(frame, total_len);

    return 1U;
}

uint8_t Boot_ParseEndFrame(uint8_t *frame, uint16_t received_len, uint32_t *packet_count)
{
    uint16_t cmd;
    uint16_t total_len;
    uint32_t count;

    /*检查输入和输出地址。*/
    if ((frame == NULL) || (packet_count == NULL))
    {
        return 0U;
    }   

    /*END帧固定为12字节。*/
    if (received_len != BOOT_END_FRAME_SIZE)
    {
        return 0U;
    }

    /*检查固定帧头。*/
    if (Boot_FrameSofValid(frame) == 0U)
    {
        return 0U;
    }

    /*检查CMD是不是结束升级命令。*/
    cmd = Boot_ParseCmd(frame);

    if (cmd != (uint16_t)CMD_END_UPDATE)
    {
        return 0U;
    }

    /*读取END帧内部声明的总长度。*/
    total_len = Boot_ParseLength(frame);

    /*长度字段必须是12，并且必须等于实际收到的长度。*/
    if ((total_len != BOOT_END_FRAME_SIZE) || (total_len != received_len))
    {
        return 0U;
    }

    /* 检查当前END帧自己的CRC*/
    if (Boot_VerifyFrameCRC(frame,total_len) == 0U)
    {
        return 0U;
    }

    /*END帧的RESERVE中保存DATA总包数*/
    count = Boot_ParseReserve(frame,total_len);

    /*已经开始了一个有效升级，DATA总包数不应该是0*/
    if (count == 0U)
    {
        return 0U;
    }

    /*所有检查通过后，才输出包数量。*/
    *packet_count = count;
    return 1U;
}

void Boot_RxInit(Boot_RxContextTypeDef *context)
{
    if (context == NULL)
    {
        return;
    }

    /*不需要清空整个buffer，received_len会限定哪些数据是有效数据。*/
    context->received_len = 0U;
    context->expected_len = 0U;
    context->frame_ready = 0U;
}

Boot_RxResultTypeDef Boot_RxInputByte(Boot_RxContextTypeDef *context,uint8_t byte)
{
    uint16_t total_len;

    if (context == NULL)
    {
        return BOOT_RX_FRAME_ERROR;
    }

    /*上一张完整帧还没有被取走和处理，此时不允许继续覆盖buffer。*/
    if(context->frame_ready != 0U)
    {
        return BOOT_RX_FRAME_READY;
    }

    /*
     * 等待帧头第一字节。
     * 帧头之前的任意噪声都直接丢弃。
     */
    if (context->received_len == 0U)
    {
        if (byte == BOOT_FRAME_SOF_BYTE0)
        {
            context->buffer[0] = byte;
            context->received_len = 1U;
        }

        return BOOT_RX_WAITING;
    }

    /*
     * 已经收到第一字节0xAA，现在等待0x55。
     * AA AA 55中的第二个AA可能是新帧头起点，因此必须保留。
     */
    if (context->received_len == 1U)
    {
        if (byte == BOOT_FRAME_SOF_BYTE1)
        {
            context->buffer[1] = byte;
            context->received_len = BOOT_FRAME_SOF_SIZE;
        }
        else if (byte == BOOT_FRAME_SOF_BYTE0)
        {
            context->buffer[0] = byte;
            context->received_len = 1U;
        }
        else
        {
            context->received_len = 0U;
        }

        return BOOT_RX_WAITING;
    }

    /*已找到完整SOF，后续字节只按TOTAL_LEN接收，不再搜索帧头。*/

    /*防止数组越界。*/
    if (context->received_len >= BOOT_FRAME_MAX_SIZE)
    {
        Boot_RxInit(context);
        return BOOT_RX_FRAME_ERROR;
    }

    /*保存当前字节，然后将已接收长度加1。*/
    context->buffer[context->received_len] = byte;
    context->received_len++;

    if(context->received_len == BOOT_FRAME_PREFIX_SIZE)
    {
        total_len = Boot_ParseLength(context->buffer);

        if((total_len < BOOT_FRAME_MIN_SIZE ||
            total_len > BOOT_FRAME_MAX_SIZE))
        {
            Boot_RxInit(context);
            return BOOT_RX_FRAME_ERROR;
        }
        
        context->expected_len = total_len;

    }

    /*
     * expected_len不为0，说明已经收到过前6字节。
     * 当实际接收长度等于目标长度时，一张帧接收完成。
     */
    if ((context->expected_len != 0U) &&
        (context->received_len ==
         context->expected_len))
    {
        context->frame_ready = 1U;
        return BOOT_RX_FRAME_READY;
    }

    
    return BOOT_RX_WAITING;
}

uint8_t Boot_RxGetFrame(Boot_RxContextTypeDef *context, uint8_t **frame, uint16_t *frame_len)
{
    if ((context == NULL) ||
        (frame == NULL) ||
        (frame_len == NULL))
    {
        return 0U;
    }

    /*当前还没有完整帧。*/
    if (context->frame_ready == 0U)
    {
        return 0U;
    }

    /*返回接收器内部buffer的地址和完整帧长度。*/
    *frame = context->buffer;
    *frame_len = context->expected_len;

    return 1U;
}

uint16_t Boot_BuildResponseFrame(
    uint8_t *frame,
    uint16_t frame_size,
    Boot_CmdTypeDef response_cmd,
    Boot_CmdTypeDef request_cmd,
    Boot_ResultTypeDef result,
    uint32_t value)
{
    uint16_t crc;
    uint16_t offset;

    /*
     * 检查输出数组地址和容量。
     */
    if ((frame == NULL) || (frame_size < BOOT_RESPONSE_FRAME_SIZE))
    {
        return 0U;
    }

    /*
     * 只允许构造ACK或者NACK。
     */
    if ((response_cmd != CMD_ACK) && (response_cmd != CMD_NACK))
    {
        return 0U;
    }

    /*frame[0～1]：固定SOF帧头。*/
    frame[BOOT_FRAME_SOF_OFFSET] = BOOT_FRAME_SOF_BYTE0;
    frame[BOOT_FRAME_SOF_OFFSET + 1U] = BOOT_FRAME_SOF_BYTE1;

    /*
     * frame[2～3]：应答命令，小端格式。
     */
    frame[BOOT_FRAME_CMD_OFFSET] =
        (uint8_t)((uint16_t)response_cmd & 0x00FFU);
    frame[BOOT_FRAME_CMD_OFFSET + 1U] =
        (uint8_t)(((uint16_t)response_cmd >> 8U) & 0x00FFU);

    /*frame[4～5]：应答帧总长度16，小端格式。*/
    frame[BOOT_FRAME_LENGTH_OFFSET] =
        (uint8_t)(BOOT_RESPONSE_FRAME_SIZE & 0x00FFU);
    frame[BOOT_FRAME_LENGTH_OFFSET + 1U] =
        (uint8_t)((BOOT_RESPONSE_FRAME_SIZE >> 8U) & 0x00FFU);

    /*
     * DATA[0～1]：原始请求命令。
     */
    offset = BOOT_FRAME_DATA_OFFSET + BOOT_RESPONSE_REQUEST_CMD_OFFSET;
    frame[offset] = (uint8_t)((uint16_t)request_cmd & 0x00FFU);
    frame[offset + 1U] =(uint8_t)(((uint16_t)request_cmd >> 8U) & 0x00FFU);

    /*
     * DATA[2～3]：处理结果码。
     */
    offset = BOOT_FRAME_DATA_OFFSET + BOOT_RESPONSE_RESULT_OFFSET;

    frame[offset] = (uint8_t)((uint16_t)result & 0x00FFU);
    frame[offset + 1U] = (uint8_t)(((uint16_t)result >> 8U) & 0x00FFU);

    /*
     * frame[10～13]：RESERVE附加值，
     * 按照uint32_t小端格式保存。
     */
    offset = BOOT_RESPONSE_RESERVE_OFFSET;
    frame[offset] = (uint8_t)(value & 0x000000FFUL);
    frame[offset + 1U] = (uint8_t)((value >> 8U) & 0x000000FFUL);
    frame[offset + 2U] = (uint8_t)((value >> 16U) & 0x000000FFUL);
    frame[offset + 3U] = (uint8_t)((value >> 24U) & 0x000000FFUL);

    /*
     * 应答CRC覆盖前14字节，包含SOF帧头，
     * 不包含最后2字节CRC字段本身。
     */
    crc = Boot_CRC16_Modbus(frame,BOOT_RESPONSE_FRAME_SIZE - BOOT_FRAME_CRC_SIZE);

    /*
     * frame[14～15]：应答帧CRC，小端格式。
     */
    offset = BOOT_RESPONSE_CRC_OFFSET;
    frame[offset] = (uint8_t)(crc & 0x00FFU);
    frame[offset + 1U] = (uint8_t)((crc >> 8U) & 0x00FFU);

    return BOOT_RESPONSE_FRAME_SIZE;
}
