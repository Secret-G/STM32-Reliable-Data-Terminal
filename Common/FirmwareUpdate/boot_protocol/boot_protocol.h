#ifndef __BOOT_PROTOCOL_H
#define __BOOT_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

/*
 * 通用协议帧各字段长度，单位均为字节。
 *
 * 帧格式：
 * SOF(2B) + CMD(2B) + TOTAL_LEN(2B) + DATA(NB)
 * + RESERVE(4B) + CRC16(2B)
 */
/*固定帧头，按照BYTE0、BYTE1的顺序在线上传输。*/
#define BOOT_FRAME_SOF_BYTE0       0xAAU
#define BOOT_FRAME_SOF_BYTE1       0x55U
#define BOOT_FRAME_SOF_SIZE        2U

/*CMD 命令字段长度*/
#define BOOT_FRAME_CMD_SIZE        2U

/*整帧总长度字段自身占用的字节数*/
#define BOOT_FRAME_LENGTH_SIZE     2U

/*保留字段长度*/
#define BOOT_FRAME_RESERVE_SIZE    4U

/*帧尾CRC16字段长度*/
#define BOOT_FRAME_CRC_SIZE        2U

/*
 * 除DATA以外的固定字段总长度：
 * 2B SOF + 2B CMD + 2B LENGTH + 4B RESERVE + 2B CRC = 12B。
 */
#define BOOT_FRAME_FIXED_SIZE      \
    (BOOT_FRAME_SOF_SIZE +         \
     BOOT_FRAME_CMD_SIZE +         \
     BOOT_FRAME_LENGTH_SIZE +      \
     BOOT_FRAME_RESERVE_SIZE +     \
     BOOT_FRAME_CRC_SIZE)

/*
 * 最小协议帧长度。
 * 当DATA长度为0时，帧中仍然包含12字节固定字段。
 */
#define BOOT_FRAME_MIN_SIZE        BOOT_FRAME_FIXED_SIZE

/* 单个协议帧允许携带的最大DATA长度。 */
#define BOOT_FRAME_MAX_DATA_SIZE   256U

/*
 * 最大协议帧长度：
 * 固定字段12字节 + 最大DATA 256字节 = 268字节。
 */
#define BOOT_FRAME_MAX_SIZE       \
    (BOOT_FRAME_FIXED_SIZE + BOOT_FRAME_MAX_DATA_SIZE)

/*
 * 通用字段在完整frame数组中的绝对偏移。
 */
#define BOOT_FRAME_SOF_OFFSET          0U
#define BOOT_FRAME_CMD_OFFSET          BOOT_FRAME_SOF_SIZE
#define BOOT_FRAME_LENGTH_OFFSET       \
    (BOOT_FRAME_CMD_OFFSET + BOOT_FRAME_CMD_SIZE)
#define BOOT_FRAME_DATA_OFFSET         \
    (BOOT_FRAME_LENGTH_OFFSET + BOOT_FRAME_LENGTH_SIZE)

/*
 * 只有收到SOF、CMD和TOTAL_LEN以后，
 * 才能知道当前完整协议帧应该有多长。
 *
 * SOF 2字节 + CMD 2字节 + TOTAL_LEN 2字节 = 6字节。
 */
#define BOOT_FRAME_PREFIX_SIZE       BOOT_FRAME_DATA_OFFSET

/*
 * CMD_START_UPDATE命令的DATA布局：
 *
 * DATA[0]    ：升级目标，1表示A区，2表示B区
 * DATA[1～4] ：BIN文件总大小，uint32_t小端
 * DATA[5～6] ：整个BIN文件的CRC16，小端
 */

/*
 * START命令DATA总长度：
 * 1B目标 + 4B文件大小 + 2B文件CRC = 7B。
 */
#define BOOT_START_DATA_SIZE           7U

/*
 * START命令整帧长度：
 * 固定字段12字节 + START DATA 7字节 = 19字节。
 */
#define BOOT_START_FRAME_SIZE          \
    (BOOT_FRAME_FIXED_SIZE + BOOT_START_DATA_SIZE)

/*
 * 升级目标在START DATA中的相对偏移。
 * 对应DATA[0]，在完整frame中对应frame[6]。
 */
#define BOOT_START_TARGET_OFFSET       0U

/*
 * BIN文件大小在START DATA中的相对偏移。
 * 占用DATA[1～4]，在完整frame中对应frame[7～10]。
 */
#define BOOT_START_IMAGE_SIZE_OFFSET   1U

/*
 * BIN整体CRC16在START DATA中的相对偏移。
 * 占用DATA[5～6]，在完整frame中对应frame[11～12]。
 */
#define BOOT_START_IMAGE_CRC_OFFSET    5U

/*
 * DATA数据帧至少携带1字节BIN数据。
 */
#define BOOT_DATA_MIN_DATA_SIZE    1U

/*
 * DATA数据帧最多携带256字节BIN数据。
 */
#define BOOT_DATA_MAX_DATA_SIZE    \
    BOOT_FRAME_MAX_DATA_SIZE

/*
 * 最小DATA帧：
 * 固定12字节 + DATA 1字节 = 13字节。
 */
#define BOOT_DATA_MIN_FRAME_SIZE   \
    (BOOT_FRAME_FIXED_SIZE + BOOT_DATA_MIN_DATA_SIZE)

/*
 * 最大DATA帧：
 * 固定12字节 + DATA 256字节 = 268字节。
 */
#define BOOT_DATA_MAX_FRAME_SIZE   \
    (BOOT_FRAME_FIXED_SIZE + BOOT_DATA_MAX_DATA_SIZE)

/*
 * END命令不携带DATA。
 */
#define BOOT_END_DATA_SIZE       0U

/*
 * END整帧长度：
 * 固定部分12字节 + DATA 0字节 = 12字节。
 */
#define BOOT_END_FRAME_SIZE      \
    (BOOT_FRAME_FIXED_SIZE + BOOT_END_DATA_SIZE)


/*
 * ACK/NACK应答帧的DATA布局：
 *
 * DATA[0～1]：原始请求命令，小端
 * DATA[2～3]：处理结果码，小端
 *
 * RESERVE字段：
 * 根据不同命令返回包序号、期望序号或总包数。
 */

/*
 * 应答帧DATA固定为4字节。
 */
#define BOOT_RESPONSE_DATA_SIZE            4U   //应答帧DATA固定为4字节。

/*
 * 应答帧总长度：
 * 固定字段12字节 + DATA 4字节 = 16字节。
 */
#define BOOT_RESPONSE_FRAME_SIZE           \
    (BOOT_FRAME_FIXED_SIZE +               \
     BOOT_RESPONSE_DATA_SIZE)

/*
 * 原始请求CMD在应答DATA中的相对偏移。
 * DATA[0～1]，对应完整帧frame[6～7]。
 */
#define BOOT_RESPONSE_REQUEST_CMD_OFFSET    0U

/*
 * 结果码在应答DATA中的相对偏移。
 * DATA[2～3]，对应完整帧frame[8～9]。
 */
#define BOOT_RESPONSE_RESULT_OFFSET         2U

/*应答帧的RESERVE和CRC绝对偏移，避免组帧时写死下标。*/
#define BOOT_RESPONSE_RESERVE_OFFSET        \
    (BOOT_FRAME_DATA_OFFSET + BOOT_RESPONSE_DATA_SIZE)
#define BOOT_RESPONSE_CRC_OFFSET            \
    (BOOT_RESPONSE_FRAME_SIZE - BOOT_FRAME_CRC_SIZE)


/*
 * 协议帧接收器的处理结果。
 */
typedef enum
{
    /* 当前还没有收够一张完整帧。 */
    BOOT_RX_WAITING = 0,

    /* 已经收够一张完整帧。 */
    BOOT_RX_FRAME_READY,

    /* 帧内长度非法，当前帧已经丢弃。 */
    BOOT_RX_FRAME_ERROR

} Boot_RxResultTypeDef;

typedef enum
{
    CMD_GET_VERSION = 0x0001,
    CMD_START_UPDATE = 0x0002,
    CMD_DATA_PACKET = 0x0003,
    CMD_END_UPDATE = 0x0004,
    CMD_CHECK_UPDATE = 0x0005,
    CMD_JUMP_APP = 0x0006,

    CMD_ACK          = 0x8000,
    CMD_NACK         = 0x8001
}Boot_CmdTypeDef;

/*
 * ACK/NACK应答中的处理结果码。
 */
typedef enum
{
    /*请求处理成功，只用于ACK。*/
    BOOT_RESULT_OK = 0x0000,

    /*协议帧长度、CMD或者帧CRC错误。*/
    BOOT_RESULT_FRAME_ERROR = 0x0001,

    /*当前升级状态不允许处理该命令。*/
    BOOT_RESULT_STATE_ERROR = 0x0002,

    /*START声明的BIN文件超过目标区域容量。*/
    BOOT_RESULT_IMAGE_TOO_LARGE = 0x0003,

    /* DATA包序号与期望序号不一致。*/
    BOOT_RESULT_SEQUENCE_ERROR = 0x0004,

    /*DATA内容超过START声明的剩余文件长度。*/
    BOOT_RESULT_DATA_TOO_LARGE = 0x0005,

    /*Flash擦除、写入或者缓存刷新失败。*/
    BOOT_RESULT_FLASH_ERROR = 0x0006,

    /*END声明的DATA总包数不正确。*/
    BOOT_RESULT_PACKET_COUNT_ERROR = 0x0007,

    /*实际接收字节数与START声明的文件大小不一致。*/
    BOOT_RESULT_IMAGE_SIZE_ERROR = 0x0008,

    /*Flash中的整个BIN CRC校验失败。*/
    BOOT_RESULT_IMAGE_CRC_ERROR = 0x0009,

    /*收到了当前Bootloader不支持的CMD。*/
    BOOT_RESULT_UNKNOWN_CMD = 0x000A,

    /*请求安装的固件版本不符合升级策略*/
    BOOT_RESULT_VERSION_ERROR = 0x000BU
} Boot_ResultTypeDef;


typedef enum
{
    BOOT_IDLE = 0,
    BOOT_ERASE,
    BOOT_RECEIVE,
    BOOT_CHECK,
    BOOT_READY,
    BOOT_ERROR
}Boot_StateTypeDef;

typedef enum
{
    /*无效或者尚未确定升级目标。*/
    UPDATE_NONE  = 0,

    /*明确指定A区。*/
    UPDATE_APP_A = 1,

    /*明确指定B区。*/
    UPDATE_APP_B = 2,

    /*不由上位机指定A/B，Bootloader根据active_slot自动选择非活动槽位。*/
    UPDATE_AUTO  = 3
}Update_TargetTypeDef;

typedef struct
{
    /* 本次升级的目标：A区或者B区。 */
    Update_TargetTypeDef target;

    /* 整个BIN文件的真实字节数。 */
    uint32_t image_size;

    /* 整个BIN文件的CRC16。 */
    uint16_t image_crc;
	
	/*START帧RESERVE中携带的固件版本,例如0x01020304表示V1.2.3.4。*/
	int32_t image_version;

} Boot_StartInfoTypeDef;

typedef struct
{
    /*指向当前协议帧中BIN数据的第一个字节，对应&frame[BOOT_FRAME_DATA_OFFSET]。*/
    uint8_t *data;

    /* 当前数据包实际携带的BIN字节数。 */
    uint16_t data_len;

    /*当前数据包序号，从0开始，从协议帧的RESERVE字段中解析。*/
    uint32_t sequence;

} Boot_DataInfoTypeDef;

typedef struct
{
    /*用来保存正在接收的完整协议帧，最大可以保存268字节。*/
    uint8_t buffer[BOOT_FRAME_MAX_SIZE];

    /*当前已经放入buffer的字节数。*/
    uint16_t received_len;

    /*从frame[4～5]解析出来的整帧目标长度，没收够前6字节时，该值为0。*/
    uint16_t expected_len;

    /*完整帧是否已经接收完成：0表示没有完成，1表示已经完成*/
    uint8_t frame_ready;

} Boot_RxContextTypeDef;

/**
 * @brief 从完整协议帧中读取2字节CMD命令。
 * @param frame 协议帧首地址。
 */
uint16_t Boot_ParseCmd(uint8_t *frame);

/**
 * @brief 从协议帧中读取2字节整帧总长度。
 * @param frame 协议帧首地址。
 * @return 整个协议帧的总字节数。
 */
uint16_t Boot_ParseLength(uint8_t *frame);

/**
 * @brief 从完整协议帧末尾读取发送方附带的CRC16。
 * @param frame     协议帧首地址。
 * @param total_len 整个协议帧的总字节数。
 */
uint16_t Boot_ParseCRC(uint8_t *frame,uint16_t total_len);

/**
 * @brief 根据整帧总长度计算DATA字段长度。
 * @param total_len 整个协议帧的总字节数。
 * @return DATA字段长度；长度非法时返回0。
 */
uint16_t Boot_GetDataLength(uint16_t total_len);

/**
 * @brief 校验一张完整协议帧的CRC16。
 * @param frame     协议帧首地址。
 * @param total_len 整个协议帧的总字节数。
 */
uint8_t Boot_VerifyFrameCRC(uint8_t *frame,uint16_t total_len);

/**
 * @brief 从完整协议帧中读取4字节RESERVE字段。
 *
 * RESERVE位于CRC前面的4个字节，采用uint32_t小端格式。
 * 后续DATA命令会把这个字段解释为数据包序号。
 *
 * @param frame     协议帧首地址。
 * @param total_len 整个协议帧的总字节数。
 *
 * @return 解析得到的32位RESERVE值；参数非法时返回0。
 */
uint32_t Boot_ParseReserve(uint8_t *frame,uint16_t total_len);

/**
 * @brief 从START命令的DATA中读取升级目标。
 *
 * DATA[0]为目标：
 * 1表示A区，2表示B区。
 *
 * @param frame START命令完整协议帧的首地址。
 *
 * @return UPDATE_APP_A、UPDATE_APP_B；
 *         参数或目标非法时返回UPDATE_NONE。
 */
Update_TargetTypeDef Boot_ParseStartTarget(uint8_t *frame);

/**
 * @brief 从START命令的DATA中读取整个BIN文件大小。
 *
 * 文件大小占用DATA[1～4]，类型为uint32_t，采用小端格式。
 *
 * @param frame START命令完整协议帧的首地址。
 *
 * @return BIN文件总字节数；frame为空时返回0。
 */
uint32_t Boot_ParseStartImageSize(uint8_t *frame);

/**
 * @brief 从START命令的DATA中读取整个BIN文件的CRC16。
 *
 * BIN整体CRC占用DATA[5～6]，采用小端格式。
 * 它用于固件全部接收完成后的整体校验，
 * 与START协议帧自身末尾的帧CRC不是同一个字段。
 *
 * @param frame START命令完整协议帧的首地址。
 *
 * @return BIN文件整体CRC16；frame为空时返回0。
 */
uint16_t Boot_ParseStartImageCRC(uint8_t *frame);

uint8_t Boot_ParseStartFrame(uint8_t *frame,uint16_t received_len,Boot_StartInfoTypeDef *start_info);

/**
 * @brief 检查并解析一张完整的DATA数据帧
 *
 * @param frame        完整协议帧首地址。
 * @param received_len 接收器实际收到的字节数。
 * @param data_info    用于保存数据地址、数据长度和包序号。
 *
 * @return 1表示解析成功，0表示DATA帧非法。
 */
uint8_t Boot_ParseDataFrame(uint8_t *frame,uint16_t received_len,Boot_DataInfoTypeDef *data_info);

/**
 * @brief 检查并解析一张END结束帧。
 *
 * @param frame        完整END协议帧首地址。
 * @param received_len 接收器实际收到的字节数。
 * @param packet_count 用于保存上位机声明的DATA总包数。
 *
 * @return 1表示解析成功，0表示END帧非法。
 */
uint8_t Boot_ParseEndFrame(uint8_t *frame,uint16_t received_len,uint32_t *packet_count);

/**
 * @brief 初始化或者复位协议帧接收器。
 *
 * @param context 协议帧接收器地址。
 */
void Boot_RxInit(Boot_RxContextTypeDef *context);

/**
 * @brief 向协议帧接收器放入一个字节。
 *
 * 先在字节流中寻找0xAA、0x55，
 * 收到前6字节后解析整帧长度，
 * 收到expected_len个字节后报告完整帧就绪。
 *
 * @param context 协议帧接收器地址。
 * @param byte    本次放入的一个串口字节。
 *
 * @return BOOT_RX_WAITING、BOOT_RX_FRAME_READY
 *         或BOOT_RX_FRAME_ERROR。
 */
Boot_RxResultTypeDef Boot_RxInputByte(Boot_RxContextTypeDef *context,uint8_t byte);

/**
 * @brief 获取已经接收完成的协议帧。
 *
 * 本函数不会复制帧数据，只返回接收器内部buffer的地址。
 *
 * @param context   协议帧接收器地址。
 * @param frame     用于输出完整帧首地址。
 * @param frame_len 用于输出完整帧长度。
 *
 * @return 1表示存在完整帧，0表示完整帧尚未准备好。
 */
uint8_t Boot_RxGetFrame(Boot_RxContextTypeDef *context,uint8_t **frame, uint16_t *frame_len);

/**
 * @brief 组装一张ACK或者NACK应答帧。
 *
 * 应答帧固定为16字节：
 * SOF(2) + CMD(2) + LEN(2) + DATA(4)
 * + RESERVE(4) + CRC(2)。
 *
 * @param frame        用于保存应答帧的数组。
 * @param frame_size   应答数组的实际容量。
 * @param response_cmd CMD_ACK或者CMD_NACK。
 * @param request_cmd  本次应答对应的原始请求CMD。
 * @param result       请求处理结果码。
 * @param value        RESERVE附加值，例如包序号。
 *
 * @return 成功返回应答帧长度16，失败返回0。
 */
uint16_t Boot_BuildResponseFrame(
    uint8_t *frame,
    uint16_t frame_size,
    Boot_CmdTypeDef response_cmd,
    Boot_CmdTypeDef request_cmd,
    Boot_ResultTypeDef result,
    uint32_t value);
    
#endif

