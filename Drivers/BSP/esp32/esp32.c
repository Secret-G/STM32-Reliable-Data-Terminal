#include "esp32.h"

#include <stdio.h>
#include <string.h>

#include "driver/esp32_driver.h"
#include "at/esp32_at_parser.h"
#include "tcp/esp32_tcp_parser.h"
#include "event/esp32_event.h"

/*
 * ESP32公共协调层
 *
 * 向下组合Driver、AT Parser和TCP Parser，向上提供稳定的AT/TCP接口。
 * 这一层负责数据分发和“发送命令—等待响应”，但不保存网络连接状态。
 */
void ESP32_AT_Init(void)
{
    ESP32_Driver_Init();
    ESP32_AT_ParserInit();
    ESP32_TCP_ParserInit();
    ESP32_Event_Init();
}

void ESP32_AT_BindTask(osThreadId_t task_id)
{
    ESP32_Driver_BindTask(task_id);
}

void ESP32_AT_FlushRx(void)
{
    ESP32_Driver_FlushRx();
    ESP32_AT_ParserClear();
}

void ESP32_AT_RecoverSync(void)
{
    static const uint8_t finish_pending_data[128] = { '\r' }; /* 补足复位前可能尚未完成的CIPSEND正文。 */
    static const uint8_t escape_sequence[3] = { '+', '+', '+' }; /* 尝试退出可能遗留的透明传输模式。 */

    printf("ESP32 AT SYNC RECOVERY\r\n");

    /* 先发送足量占位字节结束定长发送；多出的字节只会形成无效空命令。 */
    (void)ESP32_Driver_Send(finish_pending_data, sizeof(finish_pending_data));
    osDelay(100U);

    /* 保留透明传输退出所需的前后保护时间，随后丢弃恢复过程产生的响应。 */
    osDelay(1100U);
    (void)ESP32_Driver_Send(escape_sequence, sizeof(escape_sequence));
    osDelay(1100U);
    ESP32_AT_FlushRx();
}

void ESP32_AT_Process(void)
{
    uint8_t data;
    uint8_t is_tcp_payload;

    /* 同一份DMA数据分别送给AT响应解析器和TCP +IPD解析器，不再复制RingBuffer。 */
    while (ESP32_Driver_ReadByte(&data) != 0U)
    {
        /* 先识别+IPD正文，TCP正文不参与AT命令响应匹配。 */
        is_tcp_payload = ESP32_TCP_ParserFeed(data);
        if (is_tcp_payload == 0U)
        {
            ESP32_Event_Feed(data);
            ESP32_AT_ParserFeed(data);
        }
    }
}

void ESP32_AT_WaitAndProcess(uint32_t timeout_ms)
{
    (void)ESP32_AT_WaitAndProcessEvents(0U, timeout_ms);
}

uint32_t ESP32_AT_WaitAndProcessEvents(uint32_t event_flags, uint32_t timeout_ms)
{
    uint32_t flags = ESP32_Driver_WaitEvents(event_flags, timeout_ms);

    /* 自定义事件只负责唤醒；USART数据仍通过唯一DMA缓冲区读取。 */
    if ((flags & ESP32_RX_EVENT_FLAG) != 0U)
    {
        ESP32_AT_Process();
    }

    return flags;
}

HAL_StatusTypeDef ESP32_AT_SendCommand(const char *command)
{
    size_t len;

    if (command == NULL)
    {
        return HAL_ERROR;
    }

    len = strlen(command);
    if ((len == 0U) || (len > 0xFFFFU))
    {
        return HAL_ERROR;
    }

    ESP32_AT_ParserClear();
    printf("STM32 -> ESP32: %s", command);
    return ESP32_Driver_Send((const uint8_t *)command, (uint16_t)len);
}

ESP32_AT_Result_t ESP32_AT_WaitResponse(const char *expected, uint32_t timeout_ms)
{
    uint32_t flags;
    uint32_t start_tick;
    uint32_t elapsed_tick;
    uint32_t remaining_tick;

    if (expected == NULL)
    {
        return ESP32_AT_RESULT_ERROR;
    }

    /* 得到刚进入的时间 */
    start_tick = osKernelGetTickCount();

    for (;;)
    {
        /* 计算进入后经过的时间 */
        elapsed_tick = osKernelGetTickCount() - start_tick;
        
        if (elapsed_tick >= timeout_ms)
        {
            return ESP32_AT_RESULT_TIMEOUT;
        }

        /* RX事件只能唤醒处理，不能让本次命令的总超时重新计时。 */
        remaining_tick = timeout_ms - elapsed_tick;

        flags = ESP32_Driver_WaitRx(remaining_tick);
        if ((flags & ESP32_RX_EVENT_FLAG) == 0U)
        {
            return ESP32_AT_RESULT_TIMEOUT;
        }

        /* 每次被IDLE事件唤醒后消费DMA，再在累计响应中匹配结果。 */
        ESP32_AT_Process();
        
        if (ESP32_AT_ParserContains(expected) != 0U)
        {
            return ESP32_AT_RESULT_OK;
        }
        if (ESP32_AT_ParserContains("ERROR") != 0U)
        {
            return ESP32_AT_RESULT_ERROR;
        }
    }
}

ESP32_AT_Result_t ESP32_AT_Command(const char *command, const char *expected, uint32_t timeout_ms)
{
    if ((command == NULL) || (expected == NULL))
    {
        return ESP32_AT_RESULT_SEND_ERROR;
    }

    if (ESP32_AT_SendCommand(command) != HAL_OK)
    {
        return ESP32_AT_RESULT_SEND_ERROR;
    }
    return ESP32_AT_WaitResponse(expected, timeout_ms);
}

uint8_t ESP32_AT_GetIP(char *ip, uint16_t ip_size)
{
    return ESP32_AT_ParserGetIP(ip, ip_size);
}

ESP32_AT_Result_t ESP32_TCP_SendData(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    char command[32];
    int command_len;
    ESP32_AT_Result_t result;

    if ((data == NULL) || (len == 0U))
    {
        return ESP32_AT_RESULT_SEND_ERROR;
    }

    command_len = snprintf(command, sizeof(command),"AT+CIPSEND=%u\r\n", (unsigned int)len);

    if ((command_len <= 0) || ((size_t)command_len >= sizeof(command)))
    {
        return ESP32_AT_RESULT_SEND_ERROR;
    }

    /* 必须先收到'>'，它表示ESP32已经准备接收指定长度的TCP正文。 */
    result = ESP32_AT_Command(command, ">", timeout_ms);
    if (result != ESP32_AT_RESULT_OK)
    {
        return result;
    }

    ESP32_AT_ParserClear();
    if (ESP32_Driver_Send(data, len) != HAL_OK)
    {
        return ESP32_AT_RESULT_SEND_ERROR;
    }
    /* SEND OK表示ESP32已完成网络发送，而不仅是STM32的UART DMA发送完成。 */
    return ESP32_AT_WaitResponse("SEND OK", timeout_ms);
}

ESP32_AT_Result_t ESP32_TCP_SendString(const char *data,uint32_t timeout_ms)
{
    size_t len;

    if (data == NULL)
    {
        return ESP32_AT_RESULT_SEND_ERROR;
    }

    len = strlen(data);
    if ((len == 0U) || (len > 0xFFFFU))
    {
        return ESP32_AT_RESULT_SEND_ERROR;
    }
    return ESP32_TCP_SendData((const uint8_t *)data, (uint16_t)len, timeout_ms);
}
