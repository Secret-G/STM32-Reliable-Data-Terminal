#ifndef ESP32_DRIVER_H
#define ESP32_DRIVER_H

#include "main.h"
#include "cmsis_os2.h"

#define ESP32_RX_EVENT_FLAG  (1UL << 0)

/* 初始化USART3 Circular DMA接收。 */
void ESP32_Driver_Init(void);
/* 指定接收IDLE事件通知的FreeRTOS任务。 */
void ESP32_Driver_BindTask(osThreadId_t task_id);
/* 丢弃绑定任务前积累的启动日志，并清除旧RX通知。 */
void ESP32_Driver_FlushRx(void);
/* 通过USART3 TX DMA发送原始字节数据。 */
HAL_StatusTypeDef ESP32_Driver_Send(const uint8_t *data, uint16_t len);
/* 阻塞等待USART3接收事件，返回CMSIS线程标志。 */
uint32_t ESP32_Driver_WaitRx(uint32_t timeout_ms);
/* 同时等待RX和上层自定义事件；用于网络任务被待发送帧及时唤醒。 */
uint32_t ESP32_Driver_WaitEvents(uint32_t event_flags, uint32_t timeout_ms);
/* 从DMA环形缓冲区读取一个尚未处理的字节。 */
uint8_t ESP32_Driver_ReadByte(uint8_t *data);

#endif
