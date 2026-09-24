#include "esp32_tcp_parser.h"

#include <stdio.h>

/*
 * TCP下行协议解析层
 *
 * ESP32 AT固件使用 +IPD,<len>:<data> 上报TCP正文。串口数据可能被
 * 多次IDLE事件拆开，因此解析状态必须跨调用保留，不能假设一次收全。
 */
#define ESP32_TCP_RX_DATA_SIZE  512U

typedef enum
{
    ESP32_IPD_FIND_PREFIX = 0,
    ESP32_IPD_READ_LENGTH,
    ESP32_IPD_READ_DATA
} ESP32_IPD_State_t;

static ESP32_IPD_State_t esp32_ipd_state;
static uint8_t esp32_ipd_prefix_index;
static uint32_t esp32_ipd_expected_len;
static uint32_t esp32_ipd_received_len;
static uint16_t esp32_ipd_saved_len;
static uint8_t esp32_tcp_rx_data[ESP32_TCP_RX_DATA_SIZE];

static ESP32_TCP_RxHandler_t esp32_tcp_rx_handler;

void ESP32_TCP_ParserInit(void)
{
    esp32_ipd_state = ESP32_IPD_FIND_PREFIX;
    esp32_ipd_prefix_index = 0U;
    esp32_tcp_rx_handler = NULL;
}

void ESP32_TCP_RegisterRxHandler(ESP32_TCP_RxHandler_t handler)
{
    /* 显式保存应用层处理函数，避免依赖__weak链接覆盖规则 */
    esp32_tcp_rx_handler = handler;
}

uint8_t ESP32_TCP_ParserFeed(uint8_t data)
{
    static const char prefix[] = "+IPD,";
    uint8_t is_payload = 0U;

    switch (esp32_ipd_state)
    {
        case ESP32_IPD_FIND_PREFIX:

            /* 在连续字节流中寻找固定前缀“+IPD */
            if (data == (uint8_t)prefix[esp32_ipd_prefix_index])
            {
                esp32_ipd_prefix_index++;
                if (esp32_ipd_prefix_index == (sizeof(prefix) - 1U))
                {
                    esp32_ipd_prefix_index = 0U;
                    esp32_ipd_expected_len = 0U;
                    esp32_ipd_received_len = 0U;
                    esp32_ipd_saved_len = 0U;
                    esp32_ipd_state = ESP32_IPD_READ_LENGTH;
                }
            }
            else
            {
                esp32_ipd_prefix_index = (data == (uint8_t)'+') ? 1U : 0U;
            }
            break;

        case ESP32_IPD_READ_LENGTH:
            /* 冒号之前的十进制数字表示随后TCP正文的准确字节数。 */
            if ((data >= (uint8_t)'0') && (data <= (uint8_t)'9'))
            {
                esp32_ipd_expected_len = (esp32_ipd_expected_len * 10U) + (uint32_t)(data - (uint8_t)'0');
            }
            else if ((data == (uint8_t)':') && (esp32_ipd_expected_len > 0U))
            {
                /* 推进到下一个状态 */
                esp32_ipd_state = ESP32_IPD_READ_DATA;
            }
            else
            {
                esp32_ipd_state = ESP32_IPD_FIND_PREFIX;
                esp32_ipd_prefix_index = 0U;
            }
            break;

        case ESP32_IPD_READ_DATA:
            is_payload = 1U;

            /* 即使正文超过本地缓存，也继续计数到报文结束，避免解析失步。 */
            if (esp32_ipd_saved_len < ESP32_TCP_RX_DATA_SIZE)
            {
                esp32_tcp_rx_data[esp32_ipd_saved_len++] = data;
            }

            esp32_ipd_received_len++;

            if (esp32_ipd_received_len >= esp32_ipd_expected_len)
            {
                if (esp32_tcp_rx_handler != NULL)
                {
                    esp32_tcp_rx_handler(esp32_tcp_rx_data, esp32_ipd_saved_len);
                }

                if (esp32_ipd_expected_len > ESP32_TCP_RX_DATA_SIZE)
                {
                    printf("\r\n[TCP RX TRUNCATED: %lu BYTES]\r\n", (unsigned long)esp32_ipd_expected_len);
                }
                
                esp32_ipd_state = ESP32_IPD_FIND_PREFIX;
                esp32_ipd_prefix_index = 0U;
            }
            break;

        default:
            ESP32_TCP_ParserInit();
            break;
    }

    return is_payload;
}
