#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include "main.h"
#include "device_protocol.h"

typedef enum
{
    STORAGE_READ_OK = 0,       /* 成功读取并校验一条完整记录。 */
    STORAGE_READ_END,          /* upload_offset已经到达records.bin末尾。 */
    STORAGE_READ_CORRUPT,      /* 偏移、长度、帧格式或CRC不合法。 */
    STORAGE_READ_IO_ERROR      /* SD卡或FatFs读操作失败。 */
} StorageReadStatus_t;

typedef struct
{
    StorageReadStatus_t status; /* 本次异步读取的最终状态。 */
    uint32_t current_offset;    /* 当前记录在records.bin中的起始偏移。 */
    uint32_t next_offset;       /* 当前记录结束位置，ACK后才能保存为新进度。 */
    uint32_t device_id;         /* 从协议帧中解析出的设备ID。 */
    uint32_t sequence;          /* 从协议帧中解析出的数据序号。 */
    uint32_t timestamp;         /* 从协议帧中解析出的原始时间戳。 */
    uint16_t frame_length;      /* 完整编码协议帧的有效长度。 */
    uint8_t frame[DEVICE_PROTOCOL_MAX_FRAME_SIZE]; /* 后续可交给网络层的原始帧。 */
} StorageReadResult_t;

/*
 * 存储服务对外只提供“提交完整协议帧”的接口。
 * 调用者不会直接操作FatFs，真正的文件读写统一在StorageTask中完成。
 */
void StorageService_Init(void);
uint8_t StorageService_AppendFrame(const uint8_t *frame, uint16_t length);
/* 请求StorageTask从文件开头逐条读取并校验；调用者本身不操作FatFs。 */
uint8_t StorageService_RequestVerifyAll(void);
/* 请求StorageTask按当前upload_offset读取并校验一条待上传记录。 */
uint8_t StorageService_RequestReadNextPending(void);
/* 非阻塞获取StorageTask返回的一条读取结果，当前供NetworkTask消费。 */
uint8_t StorageService_TakeReadResult(StorageReadResult_t *result);

/*
 * 上传进度表示records.bin中“下一条尚未确认记录”的文件偏移。
 * Set只把请求投递给StorageTask；真正写upload.idx仍由StorageTask完成。
 * Get读取最近一次成功加载或落盘后的缓存值，不直接访问FatFs。
 */
uint8_t StorageService_SetUploadOffset(uint32_t offset);
uint8_t StorageService_GetUploadOffset(uint32_t *offset);
void StorageService_Run(void);

#endif
