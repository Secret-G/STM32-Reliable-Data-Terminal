#include "uplink_manager.h"

#include <string.h>
#include <stdio.h>

#include "cmsis_os2.h"
#include "storage_service.h"

#define UPLINK_REALTIME_QUEUE_DEPTH  4U

static osMessageQueueId_t realtime_queue; /* DataProducer向调度器提交实时帧的队列。 */
static osThreadId_t network_task_id;      /* 接收新帧事件通知的NetworkTask句柄。 */
static volatile uint8_t network_available; /* 1表示TCP已建链，可接收实时发送任务。 */
static UplinkFrame_t history_frame;       /* StorageTask最近返回的一条历史候选。 */
static uint8_t history_valid;             /* 1表示history_frame尚未被调度。 */
static UplinkFrame_t current_frame;       /* 正在发送或等待ACK的完整帧副本。 */
static uint8_t current_in_flight;         /* 1表示Network尚未报告当前帧最终结果。 */

static uint8_t UplinkManager_CopyFrame(UplinkFrame_t *destination,
                                       UplinkSource_t source,
                                       const uint8_t *frame,
                                       uint16_t length,
                                       uint32_t device_id,
                                       uint32_t sequence,
                                       uint32_t current_offset,
                                       uint32_t next_offset)
{
    if ((destination == NULL) || (frame == NULL) ||
        (length == 0U) || (length > DEVICE_PROTOCOL_MAX_FRAME_SIZE))
    {
        return 0U;
    }

    memset(destination, 0, sizeof(*destination));
    destination->source = source;
    destination->device_id = device_id;
    destination->sequence = sequence;
    destination->current_offset = current_offset;
    destination->next_offset = next_offset;
    destination->length = length;
    memcpy(destination->frame, frame, length);
    return 1U;
}

void UplinkManager_Init(void)
{
    /* 队列复制完整帧，DataProducer返回后其局部编码缓冲区可以立即复用。 */
    realtime_queue = osMessageQueueNew(UPLINK_REALTIME_QUEUE_DEPTH,
                                       sizeof(UplinkFrame_t),
                                       NULL);
    network_task_id = NULL;
    network_available = 0U;
    history_valid = 0U;
    current_in_flight = 0U;
    memset(&history_frame, 0, sizeof(history_frame));
    memset(&current_frame, 0, sizeof(current_frame));
}

void UplinkManager_BindNetworkTask(osThreadId_t task_id)
{
    network_task_id = task_id;
}

void UplinkManager_SetNetworkAvailable(uint8_t available)
{
    UplinkFrame_t queued_frame; /* 断线时从实时队列取出并转存SD的帧。 */

    network_available = (available != 0U) ? 1U : 0U;
    if ((network_available != 0U) || (realtime_queue == NULL))
    {
        return;
    }

    /* Wi-Fi不可用后，队列中的实时帧已经无法即时发送，逐条转为离线记录。 */
    while (osMessageQueueGet(realtime_queue, &queued_frame, NULL, 0U) == osOK)
    {
        if (StorageService_AppendFrame(queued_frame.frame, queued_frame.length) == 0U)
        {
            printf("UPLINK OFFLINE STORE FAILED: SEQ=%lu\r\n",
                   (unsigned long)queued_frame.sequence);
        }
    }
}

uint8_t UplinkManager_SubmitRealtime(const uint8_t *frame,
                                     uint16_t length,
                                     uint32_t device_id,
                                     uint32_t sequence)
{
    UplinkFrame_t realtime_frame; /* 准备复制到RTOS实时队列的完整消息。 */

    if (UplinkManager_CopyFrame(&realtime_frame,
                                 UPLINK_SOURCE_REALTIME,
                                 frame,
                                 length,
                                 device_id,
                                 sequence,
                                 0U,
                                 0U) == 0U)
    {
        return 0U;
    }

    /* TCP未在线时不占用RAM实时队列，直接交给StorageTask形成离线记录。 */
    if ((network_available == 0U) || (realtime_queue == NULL))
    {
        return StorageService_AppendFrame(realtime_frame.frame, realtime_frame.length);
    }

    if (osMessageQueuePut(realtime_queue, &realtime_frame, 0U, 0U) != osOK)
    {
        /* 在线但实时队列已满时同样落盘，不能静默丢弃采集数据。 */
        return StorageService_AppendFrame(realtime_frame.frame, realtime_frame.length);
    }

    if (network_task_id != NULL)
    {
        (void)osThreadFlagsSet(network_task_id, UPLINK_EVENT_FRAME_READY);
    }
    return 1U;
}

uint8_t UplinkManager_SubmitHistory(const uint8_t *frame,
                                    uint16_t length,
                                    uint32_t device_id,
                                    uint32_t sequence,
                                    uint32_t current_offset,
                                    uint32_t next_offset)
{
    uint16_t crc; /* 历史发送副本修改消息类型后重新计算的协议CRC。 */

    if ((history_valid != 0U) ||
        (UplinkManager_CopyFrame(&history_frame,
                                 UPLINK_SOURCE_HISTORY,
                                 frame,
                                 length,
                                 device_id,
                                 sequence,
                                 current_offset,
                                 next_offset) == 0U))
    {
        return 0U;
    }

    /*
     * SD中保留首次采集时的原始实时帧；这里只修改RAM发送副本。
     * 设备ID、序号、时间戳和Payload保持不变，让Qt仍能识别为同一业务数据。
     */
    history_frame.frame[DEVICE_PROTOCOL_OFFSET_TYPE] = DEVICE_MESSAGE_HISTORY_DATA;
    crc = DeviceProtocol_CalculateCrc16(history_frame.frame,
                                        (uint16_t)(history_frame.length - DEVICE_PROTOCOL_CRC_SIZE));
    history_frame.frame[history_frame.length - 2U] = (uint8_t)(crc >> 8U);
    history_frame.frame[history_frame.length - 1U] = (uint8_t)crc;

    /* 历史记录在被Network取走前保持有效，不会一次预读整个文件。 */
    history_valid = 1U;
    if (network_task_id != NULL)
    {
        (void)osThreadFlagsSet(network_task_id, UPLINK_EVENT_FRAME_READY);
    }
    return 1U;
}

uint8_t UplinkManager_TakeNext(UplinkFrame_t *frame)
{
    if ((frame == NULL) || (realtime_queue == NULL) ||
        (current_in_flight != 0U))
    {
        return 0U;
    }

    /* 每完成一帧重新选择；实时队列始终比历史候选优先。 */
    if (osMessageQueueGet(realtime_queue, &current_frame, NULL, 0U) != osOK)
    {
        if (history_valid == 0U)
        {
            return 0U;
        }

        current_frame = history_frame;
        history_valid = 0U;
    }

    current_in_flight = 1U;
    *frame = current_frame;
    return 1U;
}

uint8_t UplinkManager_CompleteCurrent(UplinkResult_t result,
                                      UplinkCompletion_t *completion)
{
    if ((completion == NULL) || (current_in_flight == 0U))
    {
        return 0U;
    }

    memset(completion, 0, sizeof(*completion));
    completion->frame = current_frame;

    if (result == UPLINK_RESULT_SUCCESS)
    {
        /* 实时成功无需额外动作；历史成功需要持久化next_offset。 */
        completion->action = (current_frame.source == UPLINK_SOURCE_HISTORY) ?
                             UPLINK_ACTION_ADVANCE_HISTORY : UPLINK_ACTION_NONE;
    }
    else
    {
        /* 实时失败时尚无可靠副本，需要落盘；历史失败保持原offset即可。 */
        completion->action = (current_frame.source == UPLINK_SOURCE_REALTIME) ?
                             UPLINK_ACTION_STORE_REALTIME : UPLINK_ACTION_NONE;
    }

    current_in_flight = 0U;
    memset(&current_frame, 0, sizeof(current_frame));
    return 1U;
}

uint8_t UplinkManager_HasInFlight(void)
{
    return current_in_flight;
}
