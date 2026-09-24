#ifndef ESP32_EVENT_H
#define ESP32_EVENT_H

#include "main.h"

/* ESP32 AT固件主动上报的异步事件。 */
#define ESP32_EVENT_NONE             0x00U
#define ESP32_EVENT_WIFI_CONNECTED   0x01U
#define ESP32_EVENT_WIFI_GOT_IP      0x02U
#define ESP32_EVENT_WIFI_DISCONNECT  0x04U
#define ESP32_EVENT_TCP_CLOSED       0x08U

void ESP32_Event_Init(void);
void ESP32_Event_Feed(uint8_t data);

/* 读取并清除尚未处理的事件；由网络任务调用。 */
uint8_t ESP32_Event_TakePending(void);

#endif
