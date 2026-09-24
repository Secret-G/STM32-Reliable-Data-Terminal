#include "esp32_event.h"

#include <string.h>

/*
 * ESP32异步事件解析层
 *
 * AT响应和异步事件共用同一条串口。这里按行收集文本，只识别网络状态，
 * 不操作UART、DMA或FreeRTOS。Wi-Fi状态采用“最后事件生效”，避免一次
 * 重连过程中的 DISCONNECT/CONNECTED/GOT IP 被同时处理而互相覆盖。
 */
#define ESP32_EVENT_LINE_SIZE  64U
#define ESP32_EVENT_WIFI_MASK  (ESP32_EVENT_WIFI_CONNECTED | \
                                ESP32_EVENT_WIFI_GOT_IP | \
                                ESP32_EVENT_WIFI_DISCONNECT)

static char event_line[ESP32_EVENT_LINE_SIZE];
static uint16_t event_line_length;
static uint8_t pending_events;

static void ESP32_Event_ParseLine(void)
{
    uint8_t wifi_event = ESP32_EVENT_NONE;

    if (strstr(event_line, "WIFI DISCONNECT") != NULL)
    {
        wifi_event = ESP32_EVENT_WIFI_DISCONNECT;
    }
    else if (strstr(event_line, "WIFI GOT IP") != NULL)
    {
        wifi_event = ESP32_EVENT_WIFI_GOT_IP;
    }
    else if (strstr(event_line, "WIFI CONNECTED") != NULL)
    {
        wifi_event = ESP32_EVENT_WIFI_CONNECTED;
    }

    if (wifi_event != ESP32_EVENT_NONE)
    {
        pending_events = (uint8_t)((pending_events & (uint8_t)(~ESP32_EVENT_WIFI_MASK)) |
                                   wifi_event);
    }

    if (strstr(event_line, "CLOSED") != NULL)
    {
        pending_events |= ESP32_EVENT_TCP_CLOSED;
    }
}

void ESP32_Event_Init(void)
{
    event_line_length = 0U;
    event_line[0] = '\0';
    pending_events = ESP32_EVENT_NONE;
}

void ESP32_Event_Feed(uint8_t data)
{
    if (data == (uint8_t)'\r')
    {
        return;
    }

    if (data == (uint8_t)'\n')
    {
        if (event_line_length > 0U)
        {
            event_line[event_line_length] = '\0';
            ESP32_Event_ParseLine();
        }
        event_line_length = 0U;
        event_line[0] = '\0';
        return;
    }

    if (event_line_length < (ESP32_EVENT_LINE_SIZE - 1U))
    {
        event_line[event_line_length++] = (char)data;
    }
    else
    {
        /* 过长行不是目标事件，直接丢弃并等待下一行。 */
        event_line_length = 0U;
        event_line[0] = '\0';
    }
}

uint8_t ESP32_Event_TakePending(void)
{
    uint8_t events = pending_events;
    pending_events = ESP32_EVENT_NONE;
    return events;
}
