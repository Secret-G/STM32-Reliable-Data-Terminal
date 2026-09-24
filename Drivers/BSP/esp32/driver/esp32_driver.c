#include "esp32_driver.h"

#include "usart.h"

/*
 * ESP32底层传输层
 *
 * 职责：
 * 1. USART3 RX由Circular DMA持续写入唯一的1024字节缓冲区；
 * 2. IDLE/DMA满缓冲事件只通知FreeRTOS任务，不在中断中处理数据；
 * 3. 任务通过NDTR计算DMA写指针，并用应用读指针逐字节消费；
 * 4. USART3 TX使用Normal DMA发送。
 *
 * 本文件不识别AT命令、OK、ERROR或+IPD，避免驱动层依赖协议层。
 */
#define ESP32_DMA_RX_BUFFER_SIZE  1024U

/* DMA硬件循环写入；应用只能通过esp32_dma_read_position读取。 */
static uint8_t esp32_dma_rx_buffer[ESP32_DMA_RX_BUFFER_SIZE];
static uint16_t esp32_dma_read_position;

/* 接收USART3事件通知的任务句柄，在任务启动后由上层绑定。 */
static osThreadId_t esp32_notify_task;

static void ESP32_Driver_StartReceive(void)
{
    esp32_dma_read_position = 0U;

    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart3, esp32_dma_rx_buffer, ESP32_DMA_RX_BUFFER_SIZE) == HAL_OK)
    {
        /* DMA负责收，关闭Half Transfer以减少无必要中断。 */
        __HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_HT);
    }
}

void ESP32_Driver_Init(void)
{
    esp32_notify_task = NULL;
    ESP32_Driver_StartReceive();
}

void ESP32_Driver_BindTask(osThreadId_t task_id)
{
    esp32_notify_task = task_id;
}

void ESP32_Driver_FlushRx(void)
{
    uint16_t dma_write_position;

    if (huart3.hdmarx != NULL)
    {
        dma_write_position = (uint16_t)(ESP32_DMA_RX_BUFFER_SIZE -  __HAL_DMA_GET_COUNTER(huart3.hdmarx));
        
        if (dma_write_position >= ESP32_DMA_RX_BUFFER_SIZE)
        {
            dma_write_position -= ESP32_DMA_RX_BUFFER_SIZE;
        }

        esp32_dma_read_position = dma_write_position;

    }

    /* 本函数由已绑定的网络任务调用，因此可安全清除当前任务的旧通知。 */
    (void)osThreadFlagsClear(ESP32_RX_EVENT_FLAG);
}

HAL_StatusTypeDef ESP32_Driver_Send(const uint8_t *data, uint16_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        return HAL_ERROR;
    }
    return HAL_UART_Transmit_DMA(&huart3, (uint8_t *)data, len);
}

uint32_t ESP32_Driver_WaitRx(uint32_t timeout_ms)
{
    return osThreadFlagsWait(ESP32_RX_EVENT_FLAG, osFlagsWaitAny, timeout_ms);
}

uint32_t ESP32_Driver_WaitEvents(uint32_t event_flags, uint32_t timeout_ms)
{
    return osThreadFlagsWait(ESP32_RX_EVENT_FLAG | event_flags,
                             osFlagsWaitAny,
                             timeout_ms);
}

uint8_t ESP32_Driver_ReadByte(uint8_t *data)
{
    uint16_t dma_write_position;

    if ((data == NULL) || (huart3.hdmarx == NULL))
    {
        return 0U;
    }

    /* NDTR保存DMA尚未传输的数量：缓冲区大小减去NDTR就是当前写位置。 */
    dma_write_position = (uint16_t)(ESP32_DMA_RX_BUFFER_SIZE - __HAL_DMA_GET_COUNTER(huart3.hdmarx));
    if (dma_write_position >= ESP32_DMA_RX_BUFFER_SIZE)
    {
        dma_write_position = 0U;
    }

    /* 读写指针相同表示当前没有尚未处理的新字节。 */
    if (esp32_dma_read_position == dma_write_position)
    {
        return 0U;
    }

    /* 只移动软件读指针；DMA写指针完全由硬件控制。 */
    *data = esp32_dma_rx_buffer[esp32_dma_read_position];
    esp32_dma_read_position++;

    if (esp32_dma_read_position >= ESP32_DMA_RX_BUFFER_SIZE)
    {
        esp32_dma_read_position = 0U;
    }
    
    return 1U;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance != USART3)
    {
        return;
    }

    /* IDLE只通知任务，不搬运、不解析、不打印。 */
    (void)size;
    if (esp32_notify_task != NULL)
    {
        (void)osThreadFlagsSet(esp32_notify_task, ESP32_RX_EVENT_FLAG);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        (void)HAL_UART_AbortReceive(huart);
        ESP32_Driver_StartReceive();
    }
}
