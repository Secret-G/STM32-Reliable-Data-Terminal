#ifndef ESP32_NETWORK_H
#define ESP32_NETWORK_H

#include "main.h"

/* 初始化网络状态机，并将当前任务绑定为ESP32事件接收任务。 */
void ESP32_Network_Init(void);

/* 执行一次当前网络状态；由FreeRTOS任务循环调用。 */
void ESP32_Network_Run(void);

#endif
