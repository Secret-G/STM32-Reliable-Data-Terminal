#ifndef __ESP32_AT_H
#define __ESP32_AT_H

#include "main.h"
#include "cmsis_os2.h"

typedef enum
{
    ESP32_AT_RESULT_OK = 0,      /* ESP32返回OK */
    ESP32_AT_RESULT_ERROR,       /* ESP32返回ERROR */
    ESP32_AT_RESULT_TIMEOUT,     /* 等待响应超时 */
    ESP32_AT_RESULT_SEND_ERROR   /* AT命令发送失败 */
} ESP32_AT_Result_t;


void ESP32_AT_Init(void);
/* 将当前网络任务与底层IDLE事件通知绑定。 */
void ESP32_AT_BindTask(osThreadId_t task_id);
/* 丢弃任务启动前ESP32产生的旧串口数据和旧通知。 */
void ESP32_AT_FlushRx(void);
/* STM32单独复位时，尝试结束ESP32遗留的透传或CIPSEND正文状态。 */
void ESP32_AT_RecoverSync(void);
/* 发送一条以'\0'结尾的AT命令字符串。 */
HAL_StatusTypeDef ESP32_AT_SendCommand(const char *command);

ESP32_AT_Result_t ESP32_AT_Command(const char *command,
                                   const char *expected,
                                   uint32_t timeout_ms);

ESP32_AT_Result_t ESP32_AT_WaitResponse(const char *expected,uint32_t timeout_ms);                                  

/* 字符串发送接口：函数内部使用strlen()自动计算正文长度。 */
ESP32_AT_Result_t ESP32_TCP_SendString(const char *data,uint32_t timeout_ms);

/* 二进制发送接口：二进制数据可能包含'\0'，必须由调用者显式提供长度。 */
ESP32_AT_Result_t ESP32_TCP_SendData(const uint8_t *data,uint16_t len,uint32_t timeout_ms);

uint8_t ESP32_AT_GetIP(char *ip, uint16_t ip_size);

void ESP32_AT_Process(void);
/* 在线阶段等待一次RX事件，并在任务上下文消费DMA数据。 */
void ESP32_AT_WaitAndProcess(uint32_t timeout_ms);
/* 等待USART RX或上层事件；如有RX仍在当前网络任务中完成DMA数据处理。 */
uint32_t ESP32_AT_WaitAndProcessEvents(uint32_t event_flags, uint32_t timeout_ms);


#endif
