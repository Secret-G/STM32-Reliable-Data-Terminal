#include "wifi_ota.h"

#include "app_update.h"
#include "boot_protocol.h"
#include "cmsis_os2.h"

#include <stdio.h>

typedef enum
{
    WIFI_OTA_IDLE = 0,
    WIFI_OTA_WAIT_BUSINESS,
    WIFI_OTA_RECEIVING,
    WIFI_OTA_PENDING_RESET
} WifiOta_State_t;

typedef struct
{
    WifiOta_State_t state;
    WifiOta_SendFrame_t send_frame;
    Boot_StartInfoTypeDef start_info;
    uint32_t device_id;
    uint32_t start_sequence;
    uint32_t last_activity_tick;
    uint32_t pending_reset_deadline;
} WifiOta_Context_t;

static WifiOta_Context_t wifi_ota;

static uint16_t WifiOta_ReadU16BE(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint32_t WifiOta_ReadU32BE(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           data[3];
}

static void WifiOta_WriteU16BE(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}

static void WifiOta_WriteU32BE(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static uint8_t WifiOta_IsMessageType(uint8_t message_type)
{
    return (uint8_t)((message_type == DEVICE_MESSAGE_OTA_START) ||
                     (message_type == DEVICE_MESSAGE_OTA_DATA) ||
                     (message_type == DEVICE_MESSAGE_OTA_END));
}

static uint8_t WifiOta_StartMatches(const DeviceProtocol_Frame_t *frame,
                                    const Boot_StartInfoTypeDef *start_info)
{
    if ((frame == NULL) || (start_info == NULL) ||
        (frame->payload_length != WIFI_OTA_START_PAYLOAD_SIZE) ||
        (frame->device_id != wifi_ota.device_id) ||
        (frame->sequence != wifi_ota.start_sequence))
    {
        return 0U;
    }

    return (uint8_t)((WifiOta_ReadU32BE(&frame->payload[0]) == start_info->image_size) &&
                     (WifiOta_ReadU16BE(&frame->payload[4]) == start_info->image_crc) &&
                     ((int32_t)WifiOta_ReadU32BE(&frame->payload[6]) == start_info->image_version));
}

static uint8_t WifiOta_SendResponse(uint8_t request_type,
                                    uint32_t sequence,
                                    Boot_ResultTypeDef result,
                                    uint32_t value)
{
    DeviceProtocol_Frame_t response;
    uint8_t encoded[DEVICE_PROTOCOL_MAX_FRAME_SIZE];
    uint16_t encoded_length;

    if (wifi_ota.send_frame == NULL)
    {
        return 0U;
    }

    DeviceProtocol_FrameInit(&response,
                             DEVICE_MESSAGE_OTA_RESPONSE,
                             wifi_ota.device_id,
                             sequence,
                             osKernelGetTickCount());
    response.payload[0] = request_type;
    WifiOta_WriteU16BE(&response.payload[1], (uint16_t)result);
    WifiOta_WriteU32BE(&response.payload[3], value);
    WifiOta_WriteU16BE(&response.payload[7], (uint16_t)WIFI_OTA_DATA_MAX_SIZE);
    response.payload_length = WIFI_OTA_RESPONSE_PAYLOAD_SIZE;

    if (DeviceProtocol_Encode(&response,
                              encoded,
                              sizeof(encoded),
                              &encoded_length) != DEVICE_PROTOCOL_OK)
    {
        return 0U;
    }

    return wifi_ota.send_frame(encoded, encoded_length);
}

static void WifiOta_ResetSession(void)
{
    wifi_ota.state = WIFI_OTA_IDLE;
    wifi_ota.device_id = 0U;
    wifi_ota.start_sequence = 0U;
    wifi_ota.last_activity_tick = 0U;
    wifi_ota.pending_reset_deadline = 0U;
}

static void WifiOta_AbortSession(const char *reason)
{
    if (wifi_ota.state == WIFI_OTA_PENDING_RESET)
    {
        return;
    }

    AppUpdate_Abort();
    printf("WIFI OTA ABORT: %s\r\n", reason);
    WifiOta_ResetSession();
}

static void WifiOta_StartNow(void)
{
    Boot_ResultTypeDef result;
    uint32_t response_value;

    result = AppUpdate_HandleStart(&wifi_ota.start_info, &response_value);
    if (result == BOOT_RESULT_OK)
    {
        wifi_ota.state = WIFI_OTA_RECEIVING;
        wifi_ota.last_activity_tick = osKernelGetTickCount();
        printf("WIFI OTA START: SIZE=%lu CRC=0x%04X VERSION=0x%08lX\r\n",
               (unsigned long)wifi_ota.start_info.image_size,
               (unsigned int)wifi_ota.start_info.image_crc,
               (unsigned long)(uint32_t)wifi_ota.start_info.image_version);
    }
    else
    {
        AppUpdate_Abort();
        wifi_ota.state = WIFI_OTA_IDLE;
    }

    (void)WifiOta_SendResponse(DEVICE_MESSAGE_OTA_START,
                               wifi_ota.start_sequence,
                               result,
                               response_value);
}

void WifiOta_Init(WifiOta_SendFrame_t send_frame)
{
    wifi_ota.send_frame = send_frame;
    AppUpdate_Init();
    WifiOta_ResetSession();
}

uint8_t WifiOta_HandleFrame(const DeviceProtocol_Frame_t *frame,
                            uint8_t business_in_flight)
{
    Boot_ResultTypeDef result;
    Boot_DataInfoTypeDef data_info;
    uint32_t response_value;
    uint32_t packet_count;
    uint32_t session_device_id;

    if ((frame == NULL) || (WifiOta_IsMessageType(frame->message_type) == 0U))
    {
        return 0U;
    }

    if (frame->message_type == DEVICE_MESSAGE_OTA_START)
    {
        if (frame->payload_length != WIFI_OTA_START_PAYLOAD_SIZE)
        {
            session_device_id = wifi_ota.device_id;
            wifi_ota.device_id = frame->device_id;
            (void)WifiOta_SendResponse(frame->message_type,
                                       frame->sequence,
                                       BOOT_RESULT_FRAME_ERROR,
                                       frame->payload_length);
            wifi_ota.device_id = session_device_id;
            return 1U;
        }

        if (wifi_ota.state != WIFI_OTA_IDLE)
        {
            if (WifiOta_StartMatches(frame, &wifi_ota.start_info) != 0U)
            {
                if (wifi_ota.state == WIFI_OTA_WAIT_BUSINESS)
                {
                    /* START响应尚未生成：保持等待，不重复初始化或擦除Flash。 */
                    printf("WIFI OTA DUPLICATE START: WAIT BUSINESS\r\n");
                    return 1U;
                }

                if (wifi_ota.state == WIFI_OTA_RECEIVING)
                {
                    result = AppUpdate_HandleStart(&wifi_ota.start_info, &response_value);
                    if (result == BOOT_RESULT_OK)
                    {
                        wifi_ota.last_activity_tick = osKernelGetTickCount();
                    }
                    (void)WifiOta_SendResponse(frame->message_type,
                                               frame->sequence,
                                               result,
                                               response_value);
                    printf("WIFI OTA DUPLICATE START: RESPONSE=%u\r\n",
                           (unsigned int)result);
                    return 1U;
                }
            }

            session_device_id = wifi_ota.device_id;
            wifi_ota.device_id = frame->device_id;
            (void)WifiOta_SendResponse(frame->message_type,
                                       frame->sequence,
                                       BOOT_RESULT_STATE_ERROR,
                                       (uint32_t)wifi_ota.state);
            wifi_ota.device_id = session_device_id;
            return 1U;
        }

        wifi_ota.device_id = frame->device_id;
        wifi_ota.start_sequence = frame->sequence;
        wifi_ota.start_info.target = UPDATE_AUTO;
        wifi_ota.start_info.image_size = WifiOta_ReadU32BE(&frame->payload[0]);
        wifi_ota.start_info.image_crc = WifiOta_ReadU16BE(&frame->payload[4]);
        wifi_ota.start_info.image_version = (int32_t)WifiOta_ReadU32BE(&frame->payload[6]);
        wifi_ota.last_activity_tick = osKernelGetTickCount();
        wifi_ota.state = WIFI_OTA_WAIT_BUSINESS;

        if (business_in_flight != 0U)
        {
            printf("WIFI OTA WAIT BUSINESS\r\n");
        }
        else
        {
            WifiOta_StartNow();
        }
        return 1U;
    }

    session_device_id = wifi_ota.device_id;
    if (((wifi_ota.state != WIFI_OTA_RECEIVING) &&
         !((wifi_ota.state == WIFI_OTA_PENDING_RESET) &&
           (frame->message_type == DEVICE_MESSAGE_OTA_END))) ||
        (frame->device_id != session_device_id))
    {
        wifi_ota.device_id = frame->device_id;
        (void)WifiOta_SendResponse(frame->message_type,
                                   frame->sequence,
                                   BOOT_RESULT_STATE_ERROR,
                                   (uint32_t)wifi_ota.state);
        wifi_ota.device_id = session_device_id;
        return 1U;
    }

    if (frame->message_type == DEVICE_MESSAGE_OTA_DATA)
    {
        if ((frame->payload_length == 0U) ||
            (frame->payload_length > WIFI_OTA_DATA_MAX_SIZE))
        {
            result = BOOT_RESULT_FRAME_ERROR;
            response_value = frame->payload_length;
        }
        else
        {
            data_info.data = (uint8_t *)frame->payload;
            data_info.data_len = frame->payload_length;
            data_info.sequence = frame->sequence;
            result = AppUpdate_HandleData(&data_info, &response_value);
        }

        /* 当前包成功或合法重复上一包时，才刷新会话活动时间。 */
        if (result == BOOT_RESULT_OK)
        {
            wifi_ota.last_activity_tick = osKernelGetTickCount();
        }

        (void)WifiOta_SendResponse(frame->message_type,
                                   frame->sequence,
                                   result,
                                   response_value);
        return 1U;
    }

    if (frame->payload_length != WIFI_OTA_END_PAYLOAD_SIZE)
    {
        result = BOOT_RESULT_FRAME_ERROR;
        response_value = frame->payload_length;
    }
    else
    {
        packet_count = WifiOta_ReadU32BE(frame->payload);
        result = AppUpdate_HandleEnd(packet_count, &response_value);
    }

    if (result == BOOT_RESULT_OK)
    {
        if (wifi_ota.state != WIFI_OTA_PENDING_RESET)
        {
            wifi_ota.state = WIFI_OTA_PENDING_RESET;
            /* PENDING已经提交：先建立最终期限，再尝试发送最终响应。 */
            wifi_ota.pending_reset_deadline = osKernelGetTickCount() +
                (uint32_t)(((uint64_t)WIFI_OTA_PENDING_RESET_TIMEOUT_MS *
                            osKernelGetTickFreq() + 999U) / 1000U);
        }
    }

    if (WifiOta_SendResponse(frame->message_type,
                             frame->sequence,
                             result,
                             response_value) != 0U)
    {
        if (wifi_ota.state == WIFI_OTA_PENDING_RESET)
        {
            printf("WIFI OTA COMPLETE, RESET\r\n");
            osDelay(WIFI_OTA_RESET_DELAY_MS);
            NVIC_SystemReset();
        }
    }

    return 1U;
}

void WifiOta_Process(uint8_t business_in_flight)
{
    uint32_t now;
    uint32_t timeout_ms;
    uint32_t timeout_ticks;

    if (wifi_ota.state == WIFI_OTA_IDLE)
    {
        return;
    }

    if ((wifi_ota.state == WIFI_OTA_WAIT_BUSINESS) &&
        (business_in_flight == 0U))
    {
        WifiOta_StartNow();
        return;
    }

    if (wifi_ota.state == WIFI_OTA_PENDING_RESET)
    {
        if ((int32_t)(osKernelGetTickCount() -
                      wifi_ota.pending_reset_deadline) >= 0)
        {
            printf("WIFI OTA PENDING RESET TIMEOUT\r\n");
            NVIC_SystemReset();
        }
        return;
    }

    now = osKernelGetTickCount();
    timeout_ms = (wifi_ota.state == WIFI_OTA_WAIT_BUSINESS) ?
                 WIFI_OTA_ENTER_TIMEOUT_MS : WIFI_OTA_SESSION_TIMEOUT_MS;
    timeout_ticks = (uint32_t)(((uint64_t)timeout_ms * osKernelGetTickFreq() + 999U) / 1000U);

    if ((uint32_t)(now - wifi_ota.last_activity_tick) >= timeout_ticks)
    {
        WifiOta_AbortSession("TIMEOUT");
    }
}

void WifiOta_OnTransportDisconnected(void)
{
    if (wifi_ota.state == WIFI_OTA_PENDING_RESET)
    {
        /* PENDING已经提交，通信断开后直接进入Bootloader，不能再恢复旧业务。 */
        NVIC_SystemReset();
    }

    if ((wifi_ota.state != WIFI_OTA_IDLE) &&
        (wifi_ota.state != WIFI_OTA_PENDING_RESET))
    {
        WifiOta_AbortSession("TRANSPORT");
    }
}

uint8_t WifiOta_IsActive(void)
{
    return (wifi_ota.state != WIFI_OTA_IDLE) ? 1U : 0U;
}
