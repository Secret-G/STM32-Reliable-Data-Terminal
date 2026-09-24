#include "esp32_network.h"
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "../esp32.h"
#include "../tcp/esp32_tcp_parser.h"
#include "../event/esp32_event.h"
#include "device_protocol.h"
#include "storage_service.h"
#include "uplink_manager.h"
#include "wifi_ota.h"

#define ESP32_ACK_TIMEOUT_MS       2000U /* 每次发送接口返回后，等待应用ACK的时长，单位ms */
#define ESP32_ACK_MAX_RETRIES      3U    /* 不含首次发送：最多额外发送3次，共4次 */
#define ESP32_CONNECT_RETRY_MS     3000U /* Wi-Fi或TCP失败后的下一次连接间隔。 */
#define ESP32_AT_RECOVER_FAILURES  2U    /* 连续AT失败达到此次数后执行一次同步恢复。 */

/*
 * ESP32网络应用层
 *
 * 负责Wi-Fi/TCP连接、已编码帧发送和应用层ACK重传，不产生业务数据。
 * ESP32_Network_Run()每次执行当前状态的一步，成功进入下一状态，
 * 连接命令失败后进入STOPPED；收到断线事件时仍会触发对应的重连流程。
 * ACK次数耗尽是ONLINE中的发送暂停，与连接失败的STOPPED状态不同。
 */
typedef enum
{
    ESP32_NET_AT_CHECK = 0,
    ESP32_NET_PREPARE,
    ESP32_NET_SET_STA_MODE,
    ESP32_NET_JOIN_WIFI,
    ESP32_NET_QUERY_IP,
    ESP32_NET_CONNECT_TCP,
    ESP32_NET_ONLINE,
    ESP32_NET_WIFI_RETRY_WAIT,
    ESP32_NET_TCP_RETRY_WAIT,
    ESP32_NET_STOPPED
} ESP32_NetState_t;

/* 1. 网络连接与调度：tick是RTOS绝对时刻，不是剩余等待时间。 */
static ESP32_NetState_t net_state;      /* 当前执行到哪一步：检测AT、连接Wi-Fi、连接TCP、在线等 */
static uint32_t next_at_check_tick;    /* 下一次允许发送AT检测命令的时刻，单位tick */
static uint32_t next_connect_retry_tick; /* Wi-Fi或TCP下一次允许重连的绝对tick。 */
static uint8_t at_check_failures;       /* 连续AT检测失败次数，用于触发一次串口同步恢复。 */
static char esp32_ip[16];              /* CIFSR查询得到的本机IPv4字符串，含末尾'\0' */

/* 2. 上行发送：帧由Uplink Manager选择，网络层只发送并处理ACK。 */
static uint8_t protocol_tx_buffer[DEVICE_PROTOCOL_MAX_FRAME_SIZE]; /* ACK完成前保留原帧，供超时重传 */
static uint16_t protocol_frame_length;

/* 3. 下行解析：TCP可能分包，拼帧过程与解析完成后的结果分别保存。 */
static DeviceProtocol_Parser_t protocol_rx_parser; /* 保存尚未收全的应用帧，跨TCP回调继续拼接 */
static DeviceProtocol_Frame_t protocol_rx_frame;   /* 拼帧并校验成功后的字段，供检查ACK类型和序号 */

/* 4. ACK控制：同一时刻只允许一张待确认帧，收到确认前不覆盖发送数组。 */
static uint32_t pending_ack_sequence; /* 当前等待Qt确认的序号；重传始终使用此序号 */
static uint32_t pending_ack_device_id; /* 当前待确认帧的设备ID，用于排除其他设备的ACK */
static uint8_t pending_ack;           /* 1=本帧尚未确认；0=可以从发送队列取下一张完整帧 */
static uint8_t ack_retries;           /* 本帧已经额外重发的次数：0~3，首次发送不计入 */
static uint32_t ack_deadline;         /* 本轮ACK等待的截止时刻，单位tick；每次发送返回后更新 */

/* 5. 历史补传调度：任何时刻最多预读一条，实时帧仍由Uplink Manager优先选择。 */
static uint8_t history_read_requested;  /* 1表示StorageTask正在处理读取请求。 */
static uint8_t history_candidate_ready; /* 1表示Uplink Manager中已有一条历史候选。 */
static uint8_t history_caught_up;       /* 1表示当前upload_offset已经到达文件末尾。 */
static uint8_t history_progress_pending;/* 1表示正在等待next_offset完成f_sync。 */
static uint32_t history_progress_target;/* 本次等待持久化的目标上传偏移。 */



static uint8_t ESP32_Network_HandlePendingAck(uint32_t now_tick);
static uint8_t ESP32_Network_StartNextFrame(void);
static void ESP32_Network_CheckStorageReadResult(void);
static void ESP32_Network_CompleteCurrent(UplinkResult_t result);
static void ESP32_Network_CheckHistoryProgress(void);
static void ESP32_Network_RequestHistoryIfNeeded(void);

static uint8_t ESP32_Network_SendOtaFrame(const uint8_t *data, uint16_t length)
{
    return (ESP32_TCP_SendData(data, length, 3000U) == ESP32_AT_RESULT_OK) ? 1U : 0U;
}
/*
 * 阅读顺序：
 * 从队列取出完整帧 -> pending_ack=1、重试次数=0 -> 发送 -> 等待ACK。
 * 正确ACK -> 将SUCCESS交给Uplink Manager -> 继续选择下一张帧。
 * 超时且次数未满 -> ack_retries加1 -> 原帧重发 -> 更新截止时刻。
 * 最后一次重传仍超时 -> 将FAILED交给Uplink Manager，不阻塞后续实时数据。
 * 实时失败帧转存SD；历史失败帧保持原上传偏移，等待以后重新读取。
 * 上述变量都在同一个网络任务中访问；接收回调也在任务中执行，不是UART中断。
 */

/* 首次发送和重传共用此函数，只发送已编码好的原帧，不生成新序号或重新编码。 */
static void ESP32_Network_SendPending(void)
{
    ESP32_AT_Result_t result = ESP32_TCP_SendData(protocol_tx_buffer,  protocol_frame_length, 3000U);

    if (result == ESP32_AT_RESULT_OK)
        printf("TCP FRAME SEND OK: SEQ=%lu LEN=%u\r\n",
               (unsigned long)pending_ack_sequence, (unsigned int)protocol_frame_length);
    else
        printf("TCP SEND FAILED: SEQ=%lu, WAIT ACK/RETRY\r\n",
               (unsigned long)pending_ack_sequence);
    /*
     * 将毫秒换算成RTOS tick，+999用于向上取整，然后加当前时刻得到截止时刻。
     * 公式：(ms × TickFreq + 999) / 1000
     */
    ack_deadline = osKernelGetTickCount() +
        (uint32_t)(((uint64_t)ESP32_ACK_TIMEOUT_MS * osKernelGetTickFreq() + 999U) / 1000U);

}

static void ESP32_Network_HandleEvents(void)
{
    uint8_t events = ESP32_Event_TakePending();

    if ((events & ESP32_EVENT_WIFI_DISCONNECT) != 0U)
    {
        printf("[EVENT] WIFI DISCONNECT\r\n");
        if (pending_ack != 0U)
        {
            pending_ack = 0U;
            ESP32_Network_CompleteCurrent(UPLINK_RESULT_FAILED);
        }
        UplinkManager_SetNetworkAvailable(0U);
        WifiOta_OnTransportDisconnected();
        /* 断线时结束当前在途帧：实时帧落盘，历史帧保持原上传偏移。 */
        DeviceProtocol_ParserReset(&protocol_rx_parser);
        next_connect_retry_tick = osKernelGetTickCount() + ESP32_CONNECT_RETRY_MS;
        net_state = ESP32_NET_WIFI_RETRY_WAIT;
        return;
    }

    if ((events & ESP32_EVENT_WIFI_CONNECTED) != 0U)
    {
        printf("[EVENT] WIFI CONNECTED\r\n");
    }

    if ((events & ESP32_EVENT_WIFI_GOT_IP) != 0U)
    {
        printf("[EVENT] WIFI GOT IP\r\n");
        if ((net_state == ESP32_NET_STOPPED) ||
            (net_state == ESP32_NET_WIFI_RETRY_WAIT))
        {
            net_state = ESP32_NET_QUERY_IP;
        }
    }

    if ((events & ESP32_EVENT_TCP_CLOSED) != 0U)
    {
        printf("[EVENT] TCP CLOSED\r\n");
        if (pending_ack != 0U)
        {
            pending_ack = 0U;
            ESP32_Network_CompleteCurrent(UPLINK_RESULT_FAILED);
        }
        UplinkManager_SetNetworkAvailable(0U);
        WifiOta_OnTransportDisconnected();
        DeviceProtocol_ParserReset(&protocol_rx_parser);
        next_connect_retry_tick = osKernelGetTickCount() + ESP32_CONNECT_RETRY_MS;
        net_state = ESP32_NET_TCP_RETRY_WAIT;
    }
}

static void ESP32_Network_OnTcpData(const uint8_t *data, uint16_t len)
{
    uint16_t index;

    /* 回调在网络任务中执行；TCP分包/粘包交给应用协议拼帧处理。 */
    for (index = 0U; index < len; index++)
    {
        if (DeviceProtocol_ParserFeed(&protocol_rx_parser, data[index], &protocol_rx_frame) == 0U)
            continue;

        /* OTA适配层优先识别自己的消息，普通业务帧继续走原有逻辑。 */
        if (WifiOta_HandleFrame(&protocol_rx_frame, pending_ack) != 0U)
        {
            continue;
        }

            /* 拼帧成功，检查是否是应用ACK帧。 */
        if ((protocol_rx_frame.message_type == DEVICE_MESSAGE_ACK) &&
            (protocol_rx_frame.payload_length == 0U) &&
            (protocol_rx_frame.device_id == pending_ack_device_id))
        {
            /* 收到ACK帧，检查序号是否匹配当前待确认帧。 */
            if ((pending_ack != 0U) && (protocol_rx_frame.sequence == pending_ack_sequence))
            {
                /* 确认的是当前这张帧：结束等待。计数在下一张新帧建立时再清零。 */
                pending_ack = 0U;
                ESP32_Network_CompleteCurrent(UPLINK_RESULT_SUCCESS);
                printf("APP ACK SEQ=%lu\r\n", (unsigned long)protocol_rx_frame.sequence);
            }
            else
            {
                printf("APP ACK UNMATCHED SEQ=%lu\r\n", (unsigned long)protocol_rx_frame.sequence);
            }
        }
    }
}

void ESP32_Network_Init(void)
{
    /* 当前网络任务是唯一等待ESP32接收事件的任务。 */
    ESP32_AT_BindTask(osThreadGetId());
    ESP32_AT_FlushRx();
    ESP32_TCP_RegisterRxHandler(ESP32_Network_OnTcpData);
    UplinkManager_BindNetworkTask(osThreadGetId());
    UplinkManager_SetNetworkAvailable(0U);
    net_state = ESP32_NET_AT_CHECK;
    next_at_check_tick = osKernelGetTickCount();
    next_connect_retry_tick = 0U;
    at_check_failures = 0U;
    pending_ack = 0U;
    ack_retries = 0U;
    history_read_requested = 0U;
    history_candidate_ready = 0U;
    history_caught_up = 0U;
    history_progress_pending = 0U;
    history_progress_target = 0U;
    DeviceProtocol_ParserReset(&protocol_rx_parser);
    WifiOta_Init(ESP32_Network_SendOtaFrame);
}

void ESP32_Network_Run(void)
{
    ESP32_AT_Result_t result;
    uint32_t now_tick;

    /* 先应用ESP32主动上报的状态，再执行本轮网络状态机。 */
    ESP32_Network_HandleEvents();

    switch (net_state)
    {
        case ESP32_NET_AT_CHECK:
            now_tick = osKernelGetTickCount();
            if ((int32_t)(now_tick - next_at_check_tick) < 0)
            {
                /* AT检测失败后只等待下一次检测，不重跑整套网络流程。 */
                ESP32_AT_WaitAndProcess(next_at_check_tick - now_tick);
                break;
            }

            result = ESP32_AT_Command("AT\r\n", "OK", 2000U);
            if (result == ESP32_AT_RESULT_OK)
            {
                printf("ESP32 AT CHECK OK\r\n");
                at_check_failures = 0U;
                net_state = ESP32_NET_PREPARE;
            }
            else
            {
                printf("ESP32 AT CHECK FAILED\r\n");
                at_check_failures++;
                if (at_check_failures >= ESP32_AT_RECOVER_FAILURES)
                {
                    /* 连续无响应通常表示ESP32仍停留在STM32复位前的发送状态。 */
                    ESP32_AT_RecoverSync();
                    at_check_failures = 0U;
                }
                next_at_check_tick = osKernelGetTickCount() + ESP32_CONNECT_RETRY_MS;
            }
            break;

        case ESP32_NET_PREPARE:
            /* 关闭命令回显，减少响应中的无关字符。 */
            result = ESP32_AT_Command("ATE0\r\n", "OK", 1000U);
            if (result != ESP32_AT_RESULT_OK)
            {
                printf("ESP32 DISABLE ECHO FAILED\r\n");
                next_at_check_tick = osKernelGetTickCount() + ESP32_CONNECT_RETRY_MS;
                net_state = ESP32_NET_AT_CHECK;
                break;
            }

            /*
             * STM32复位时ESP32可能仍保留旧TCP连接。
             * 无连接时CIPCLOSE返回ERROR也属于正常情况，因此这里只负责清理，不判断结果。
             */
            (void)ESP32_AT_Command("AT+CIPCLOSE\r\n", "OK", 1000U);
            net_state = ESP32_NET_SET_STA_MODE;
            break;

        case ESP32_NET_SET_STA_MODE:
            result = ESP32_AT_Command("AT+CWMODE=1\r\n", "OK", 2000U);
            net_state = (result == ESP32_AT_RESULT_OK) ? ESP32_NET_JOIN_WIFI : ESP32_NET_AT_CHECK;
            if (result != ESP32_AT_RESULT_OK)
            {
                next_at_check_tick = osKernelGetTickCount() + ESP32_CONNECT_RETRY_MS;
            }
            printf((result == ESP32_AT_RESULT_OK) ?  "ESP32 STA MODE OK\r\n" : "ESP32 SET STA MODE FAILED\r\n");
            break;

        case ESP32_NET_JOIN_WIFI:
            result = ESP32_AT_Command("AT+CWJAP=\"123\",\"chen92516\"\r\n","OK", 15000U);
            net_state = (result == ESP32_AT_RESULT_OK) ? ESP32_NET_QUERY_IP : ESP32_NET_WIFI_RETRY_WAIT;
            if (result != ESP32_AT_RESULT_OK)
            {
                next_connect_retry_tick = osKernelGetTickCount() + ESP32_CONNECT_RETRY_MS;
            }
            printf((result == ESP32_AT_RESULT_OK) ? "ESP32 WIFI CONNECT OK\r\n" : "ESP32 WIFI CONNECT FAILED\r\n");
            break;

        case ESP32_NET_QUERY_IP:
            result = ESP32_AT_Command("AT+CIFSR\r\n", "OK", 2000U);
            if ((result == ESP32_AT_RESULT_OK) && (ESP32_AT_GetIP(esp32_ip, sizeof(esp32_ip)) != 0U))
            {
                printf("ESP32 IP: %s\r\n", esp32_ip);
                net_state = ESP32_NET_CONNECT_TCP;
            }
            else
            {
                printf("ESP32 QUERY OR PARSE IP FAILED\r\n");
                next_connect_retry_tick = osKernelGetTickCount() + ESP32_CONNECT_RETRY_MS;
                net_state = ESP32_NET_WIFI_RETRY_WAIT;
            }
            break;

        case ESP32_NET_CONNECT_TCP:
            result = ESP32_AT_Command("AT+CIPSTART=\"TCP\",\"10.34.173.87\",8080\r\n", "OK", 5000U);
            if (result == ESP32_AT_RESULT_OK)
            {
                printf("ESP32 TCP CONNECT OK\r\n");
                net_state = ESP32_NET_ONLINE;
                UplinkManager_SetNetworkAvailable(1U);
                /* 每次重连都重新检查SD，断网期间可能新增了离线记录。 */
                history_caught_up = 0U;
            }
            else
            {
                printf("ESP32 TCP CONNECT FAILED\r\n");
                UplinkManager_SetNetworkAvailable(0U);
                next_connect_retry_tick = osKernelGetTickCount() + ESP32_CONNECT_RETRY_MS;
                net_state = ESP32_NET_TCP_RETRY_WAIT;
            }
            break;

        case ESP32_NET_ONLINE:
            /* ONLINE状态同时处理上行发送队列和服务器下行数据。 */
            /* 接收StorageTask异步结果，并确认上一次历史ACK对应的进度已经落盘。 */
            ESP32_Network_CheckStorageReadResult();
            ESP32_Network_CheckHistoryProgress();
            /* 先消费已到达的数据，避免在截止时间边界漏看ACK而额外重发。 */
            ESP32_AT_Process();
            ESP32_Network_HandleEvents();

            if (net_state != ESP32_NET_ONLINE) break;
            now_tick = osKernelGetTickCount();
            
            if(ESP32_Network_HandlePendingAck(now_tick) != 0U)
            {
                break;
            }

            WifiOta_Process(pending_ack);
            if (WifiOta_IsActive() != 0U)
            {
                /* OTA期间继续消费ESP-AT接收，但不再取新的实时/历史上行帧。 */
                ESP32_AT_WaitAndProcess(100U);
                break;
            }

            ESP32_Network_RequestHistoryIfNeeded();
            if (ESP32_Network_StartNextFrame() == 0U)
            {
                /* 同时等待DMA接收和新发送帧通知，避免固定周期轮询。 */
                (void)ESP32_AT_WaitAndProcessEvents(UPLINK_EVENT_FRAME_READY, 1000U);
            }
            break;

        case ESP32_NET_WIFI_RETRY_WAIT:
            now_tick = osKernelGetTickCount();
            if ((int32_t)(now_tick - next_connect_retry_tick) >= 0)
            {
                printf("ESP32 WIFI RETRY\r\n");
                net_state = ESP32_NET_JOIN_WIFI;
            }
            else
            {
                ESP32_AT_WaitAndProcess(next_connect_retry_tick - now_tick);
            }
            break;

        case ESP32_NET_TCP_RETRY_WAIT:
            now_tick = osKernelGetTickCount();
            if ((int32_t)(now_tick - next_connect_retry_tick) >= 0)
            {
                printf("ESP32 TCP RETRY\r\n");
                net_state = ESP32_NET_CONNECT_TCP;
            }
            else
            {
                ESP32_AT_WaitAndProcess(next_connect_retry_tick - now_tick);
            }
            break;

        case ESP32_NET_STOPPED:
            /*
             * 停止状态仍要消费USART3 DMA数据，否则WIFI GOT IP等异步事件
             * 只停留在DMA缓冲区中，Event Parser永远无法看到它们。
             */
            ESP32_AT_WaitAndProcess(ESP32_CONNECT_RETRY_MS);
            next_at_check_tick = osKernelGetTickCount();
            net_state = ESP32_NET_AT_CHECK;
            break;

        default:
            net_state = ESP32_NET_STOPPED;
            break;
    }
}

static void ESP32_Network_CheckStorageReadResult(void)
{
    StorageReadResult_t read_result; /* StorageTask返回的完整历史读取结果副本。 */

    while (StorageService_TakeReadResult(&read_result) != 0U)
    {
        history_read_requested = 0U;

        if (read_result.status == STORAGE_READ_OK)
        {
            /* 只预取一条历史记录，真正发送顺序由Uplink Manager统一决定。 */
            if (UplinkManager_SubmitHistory(read_result.frame,
                                            read_result.frame_length,
                                            read_result.device_id,
                                            read_result.sequence,
                                            read_result.current_offset,
                                            read_result.next_offset) != 0U)
            {
                history_candidate_ready = 1U;
                history_caught_up = 0U;
                printf("HISTORY FRAME READY: OFFSET=%lu NEXT=%lu SEQ=%lu LEN=%u\r\n",
                       (unsigned long)read_result.current_offset,
                       (unsigned long)read_result.next_offset,
                       (unsigned long)read_result.sequence,
                       (unsigned int)read_result.frame_length);
            }
            else
            {
                printf("HISTORY FRAME SUBMIT FAILED: OFFSET=%lu SEQ=%lu\r\n",
                       (unsigned long)read_result.current_offset,
                       (unsigned long)read_result.sequence);
            }
        }
        else if (read_result.status == STORAGE_READ_END)
        {
            history_caught_up = 1U;
            printf("NETWORK STORAGE CAUGHT UP: OFFSET=%lu\r\n",
                   (unsigned long)read_result.current_offset);
        }
        else
        {
            /* 损坏记录无法安全寻找下一条边界，停止本轮补传等待人工检查。 */
            history_caught_up = 1U;
            printf("NETWORK STORAGE READ INVALID: STATUS=%d OFFSET=%lu\r\n",
                   (int)read_result.status,
                   (unsigned long)read_result.current_offset);
        }
    }
}

static void ESP32_Network_CheckHistoryProgress(void)
{
    uint32_t saved_offset; /* StorageTask最近一次完成f_sync后的缓存进度。 */

    if ((history_progress_pending == 0U) ||
        (StorageService_GetUploadOffset(&saved_offset) == 0U) ||
        (saved_offset != history_progress_target))
    {
        return;
    }

    printf("HISTORY PROGRESS CONFIRMED: OFFSET=%lu\r\n",
           (unsigned long)saved_offset);
    history_progress_pending = 0U;
    history_caught_up = 0U;
}

static void ESP32_Network_RequestHistoryIfNeeded(void)
{
    if ((net_state != ESP32_NET_ONLINE) ||
        (history_read_requested != 0U) ||
        (history_candidate_ready != 0U) ||
        (history_progress_pending != 0U) ||
        (history_caught_up != 0U))
    {
        return;
    }

    /* 一次只向StorageTask请求一条，避免历史读取占满RAM和任务队列。 */
    if (StorageService_RequestReadNextPending() != 0U)
    {
        history_read_requested = 1U;
    }
    else
    {
        printf("HISTORY READ REQUEST FAILED\r\n");
    }
}

static void ESP32_Network_CompleteCurrent(UplinkResult_t result)
{
    UplinkCompletion_t completion; /* Uplink Manager返回的最终处置动作和原帧。 */

    if (UplinkManager_CompleteCurrent(result, &completion) == 0U)
    {
        return;
    }

    if (completion.action == UPLINK_ACTION_STORE_REALTIME)
    {
        /* 实时帧最终失败后没有可靠副本，交给StorageTask异步落盘。 */
        if (StorageService_AppendFrame(completion.frame.frame,
                                       completion.frame.length) != 0U)
        {
            /* 记录已排队，后续历史读请求会在StorageTask中按顺序执行。 */
            history_caught_up = 0U;
        }
        else
        {
            printf("UPLINK FAILED FRAME STORE QUEUE FULL: SEQ=%lu\r\n",
                   (unsigned long)completion.frame.sequence);
        }
    }
    else if (completion.action == UPLINK_ACTION_ADVANCE_HISTORY)
    {
        /* 历史帧只有收到匹配ACK后，才允许提交下一条记录偏移。 */
        if (StorageService_SetUploadOffset(completion.frame.next_offset) != 0U)
        {
            /* 等StorageTask写入并f_sync成功后，再请求下一条历史记录。 */
            history_progress_target = completion.frame.next_offset;
            history_progress_pending = 1U;
        }
        else
        {
            printf("UPLOAD PROGRESS REQUEST FAILED: SEQ=%lu NEXT=%lu\r\n",
                   (unsigned long)completion.frame.sequence,
                   (unsigned long)completion.frame.next_offset);
            history_caught_up = 0U;
        }
    }
    else if ((result == UPLINK_RESULT_FAILED) &&
             (completion.frame.source == UPLINK_SOURCE_HISTORY))
    {
        /* 历史帧原文仍在SD中：失败时不改offset，下次仍读同一条。 */
        history_progress_pending = 0U;
        history_caught_up = 0U;
    }
}


static uint8_t ESP32_Network_HandlePendingAck(uint32_t now_tick)
{
    if (pending_ack == 0U)
    {
        return 0U;
    }

    if ((int32_t)(now_tick - ack_deadline) < 0)
    {
        uint32_t wait_ms;

        wait_ms = (uint32_t)(((uint64_t)(ack_deadline - now_tick) *
                              1000U +
                              osKernelGetTickFreq() - 1U) /
                             osKernelGetTickFreq());

        ESP32_AT_WaitAndProcess(wait_ms);
        return 1U;
    }

    if (ack_retries < ESP32_ACK_MAX_RETRIES)
    {
        ack_retries++;

        printf("APP ACK TIMEOUT: SEQ=%lu RETRY=%u/%u\r\n",
               (unsigned long)pending_ack_sequence,
               (unsigned int)ack_retries,
               (unsigned int)ESP32_ACK_MAX_RETRIES);

        ESP32_Network_SendPending();
        return 1U;
    }

    printf("APP ACK FAILED: SEQ=%lu, HAND OVER TO UPLINK\r\n",
           (unsigned long)pending_ack_sequence);

    /* 重试耗尽是当前帧最终失败，不再永久阻塞后续实时数据。 */
    pending_ack = 0U;
    ESP32_Network_CompleteCurrent(UPLINK_RESULT_FAILED);

    return 1U;
}

static uint8_t ESP32_Network_StartNextFrame(void)
{
    UplinkFrame_t frame; /* Uplink Manager按实时优先规则选出的下一张完整帧。 */

    if (UplinkManager_TakeNext(&frame) == 0U)
    {
        return 0U;
    }

    if (frame.source == UPLINK_SOURCE_HISTORY)
    {
        /* 历史候选已转为在途帧，ACK结束前不再预读下一条。 */
        history_candidate_ready = 0U;
    }

    memcpy(protocol_tx_buffer, frame.frame, frame.length);
    protocol_frame_length = frame.length;
    pending_ack_device_id = frame.device_id;
    pending_ack_sequence = frame.sequence;
    pending_ack = 1U;
    ack_retries = 0U;

    printf("TX FRAME SEQ=%lu SOURCE=%s\r\n",
           (unsigned long)pending_ack_sequence,
           (frame.source == UPLINK_SOURCE_REALTIME) ? "REALTIME" : "HISTORY");

    ESP32_Network_SendPending();
    return 1U;
}
