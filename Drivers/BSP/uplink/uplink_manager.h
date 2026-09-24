#ifndef UPLINK_MANAGER_H
#define UPLINK_MANAGER_H

#include "main.h"
#include "cmsis_os2.h"
#include "device_protocol.h"

#define UPLINK_EVENT_FRAME_READY  (1UL << 1)

/* 当前帧来自实时采集还是SD历史记录，最终失败时两者处理方式不同。 */
typedef enum
{
    UPLINK_SOURCE_REALTIME = 0,
    UPLINK_SOURCE_HISTORY
} UplinkSource_t;

/* ESP32 Network完成内部ACK重传后，只向调度层报告最终结果。 */
typedef enum
{
    UPLINK_RESULT_SUCCESS = 0,
    UPLINK_RESULT_FAILED
} UplinkResult_t;

/* Uplink Manager根据来源和发送结果，决定上层下一步需要执行的动作。 */
typedef enum
{
    UPLINK_ACTION_NONE = 0,
    UPLINK_ACTION_STORE_REALTIME,
    UPLINK_ACTION_ADVANCE_HISTORY
} UplinkAction_t;

typedef struct
{
    UplinkSource_t source; /* 帧来源决定失败后落盘还是保留原历史记录。 */
    uint32_t device_id;    /* ACK匹配所需的设备ID。 */
    uint32_t sequence;     /* ACK匹配和Qt幂等去重所需的业务序号。 */
    uint32_t current_offset; /* 历史帧在records.bin中的起始位置。 */
    uint32_t next_offset;    /* 历史帧ACK成功后应保存的新上传位置。 */
    uint16_t length;         /* frame数组中的完整编码帧长度。 */
    uint8_t frame[DEVICE_PROTOCOL_MAX_FRAME_SIZE]; /* 可直接交给Network发送的协议帧。 */
} UplinkFrame_t;

typedef struct
{
    UplinkAction_t action; /* 最终发送结果对应的后续动作。 */
    UplinkFrame_t frame;   /* 动作所对应的原始在途帧副本。 */
} UplinkCompletion_t;

/* 创建实时队列并清空历史槽、在途状态，需在任务创建前调用。 */
void UplinkManager_Init(void);

/* 绑定实际执行发送的NetworkTask，用事件标志及时唤醒网络任务。 */
void UplinkManager_BindNetworkTask(osThreadId_t task_id);

/* Network连接状态变化时更新可用性；离线时排队实时帧会转存SD。 */
void UplinkManager_SetNetworkAvailable(uint8_t available);

/* DataProducer使用此接口提交实时帧；接口不执行TCP发送和文件操作。 */
uint8_t UplinkManager_SubmitRealtime(const uint8_t *frame,
                                     uint16_t length,
                                     uint32_t device_id,
                                     uint32_t sequence);

/* Storage读取结果后提交一条历史候选；同一时刻只保留一条待调度历史帧。 */
uint8_t UplinkManager_SubmitHistory(const uint8_t *frame,
                                    uint16_t length,
                                    uint32_t device_id,
                                    uint32_t sequence,
                                    uint32_t current_offset,
                                    uint32_t next_offset);

/* Network空闲时获取下一帧：始终先取实时队列，再考虑历史候选。 */
uint8_t UplinkManager_TakeNext(UplinkFrame_t *frame);

/* Network报告最终ACK结果，并取得需要落盘或推进进度的动作。 */
uint8_t UplinkManager_CompleteCurrent(UplinkResult_t result,
                                      UplinkCompletion_t *completion);

/* 用于判断Network是否已有一帧正在发送或等待ACK。 */
uint8_t UplinkManager_HasInFlight(void);

#endif
