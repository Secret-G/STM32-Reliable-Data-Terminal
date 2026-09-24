#include "data_producer.h"

#include <stdio.h>

#include "main.h"
#include "cmsis_os2.h"
#include "device_protocol.h"
#include "uplink_manager.h"

#define DATA_DEVICE_ID            0x00000001UL
#define DATA_TEST_VALUE           0x12345678UL
#define DATA_PRODUCE_PERIOD_MS    5000U
#define DATA_SEQUENCE_MAGIC       0x53455131UL /* RTC备份寄存器标记：ASCII SEQ1。 */

static void DataProducer_SequenceInit(void)
{
    /* 备份寄存器不会像Flash那样产生擦写磨损，适合每帧保存下一个序号。 */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    /*
     * BKP0R由Bootloader的Trial/Confirm握手独占。
     * DataProducer使用BKP2R保存magic，BKP1R保存下一个业务序号。
     * 从旧版本迁移时BKP2R尚未设置，但BKP1R可能已有有效序号，必须保留。
     */
    if (RTC->BKP2R != DATA_SEQUENCE_MAGIC)
    {
        RTC->BKP2R = DATA_SEQUENCE_MAGIC;
    }

    if ((RTC->BKP1R == 0U) || (RTC->BKP1R == 0xFFFFFFFFUL))
    {
        RTC->BKP1R = 1U;
    }

    __DSB();
}

static uint32_t DataProducer_ReserveSequence(void)
{
    uint32_t current_sequence = RTC->BKP1R; /* 本帧使用的持久序号。 */
    uint32_t next_sequence = current_sequence + 1U; /* 复位后从这里继续分配。 */

    if ((next_sequence == 0U) || (next_sequence == 0xFFFFFFFFUL))
    {
        next_sequence = 1U;
    }

    /* 先保存下一值再构造和提交本帧；突然复位最多跳号，不会复用已发送序号。 */
    RTC->BKP1R = next_sequence;
    __DSB();
    return current_sequence;
}

void DataProducer_Run(void)
{
    DeviceProtocol_Frame_t frame;  /* 本周期待编码的结构化协议帧。 */
    DeviceProtocol_Result_t result;/* Payload填充和协议编码的执行结果。 */
    uint8_t encoded_frame[DEVICE_PROTOCOL_MAX_FRAME_SIZE]; /* 存储与网络共用的完整编码帧。 */
    uint16_t encoded_length;       /* 当前完整编码帧的有效字节数。 */
    uint32_t sequence = 1U;        /* 当前上行序号，现阶段上电后从1开始。 */
    uint32_t timestamp;            /* 产生本帧时的RTOS tick时间戳。 */
    uint32_t next_wake_tick = osKernelGetTickCount(); /* 下一次绝对唤醒时刻。 */
    uint32_t period_ticks = (uint32_t)(((uint64_t)DATA_PRODUCE_PERIOD_MS *
                                        osKernelGetTickFreq() + 999U) / 1000U);

    DataProducer_SequenceInit();
    printf("DATA SEQUENCE START: NEXT=%lu\r\n", (unsigned long)RTC->BKP1R);

    for (;;)
    {
        /* sequence声明保留默认值，实际每周期从RTC备份域领取一个不会因软件复位重复的值。 */
        sequence = DataProducer_ReserveSequence();
        /* 时间戳和序号在数据产生处确定，之后存储、发送和重传都使用同一张帧。 */
        timestamp = osKernelGetTickCount();
        DeviceProtocol_FrameInit(&frame,
                                 DEVICE_MESSAGE_REALTIME_DATA,
                                 DATA_DEVICE_ID,
                                 sequence,
                                 timestamp);

        result = DeviceProtocol_SetRealtimeValue(&frame, DATA_TEST_VALUE);
        if (result == DEVICE_PROTOCOL_OK)
        {
            result = DeviceProtocol_Encode(&frame, encoded_frame, sizeof(encoded_frame), &encoded_length);
        }

        if (result != DEVICE_PROTOCOL_OK)
        {
            printf("DATA FRAME BUILD FAILED: %d\r\n", (int)result);
        }
        else
        {
            /* DataProducer只提交一次，由Uplink Manager决定实时发送或离线落盘。 */
            if (UplinkManager_SubmitRealtime(encoded_frame,
                                             encoded_length,
                                             DATA_DEVICE_ID,
                                             sequence) == 0U)
            {
                printf("UPLINK SUBMIT FAILED: SEQ=%lu\r\n", (unsigned long)sequence);
            }

            printf("DATA FRAME CREATED: SEQ=%lu TIME=%lu LEN=%u\r\n",
                   (unsigned long)sequence,
                   (unsigned long)timestamp,
                   (unsigned int)encoded_length);
            sequence++;
        }

        /* 使用绝对唤醒时刻，避免编码和投递耗时逐周期累积造成采样漂移。 */
        next_wake_tick += period_ticks;
        (void)osDelayUntil(next_wake_tick);
    }
}
