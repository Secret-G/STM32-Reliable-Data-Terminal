#include "esp32_at_parser.h"

#include <string.h>

/*
 * AT响应解析层
 *
 * 这里只保存并查询当前命令的文本响应，不直接操作UART、DMA或RTOS。
 * 协调层把收到的每个字节送入ESP32_AT_ParserFeed()，随后可使用
 * Contains()判断期望响应，或使用GetIP()提取CIFSR返回的IP地址。
 */
#define ESP32_AT_RESPONSE_SIZE  512U

static char esp32_at_response[ESP32_AT_RESPONSE_SIZE];
static uint16_t esp32_at_response_len;

void ESP32_AT_ParserInit(void)
{
    ESP32_AT_ParserClear();
}

void ESP32_AT_ParserClear(void)
{
    esp32_at_response_len = 0U;
    esp32_at_response[0] = '\0';
}

void ESP32_AT_ParserFeed(uint8_t data)
{
    /* 始终给C字符串保留末尾'\0'，保证strstr()/strchr()可安全使用。 */
    if (esp32_at_response_len < (ESP32_AT_RESPONSE_SIZE - 1U))
    {
        esp32_at_response[esp32_at_response_len++] = (char)data;
        esp32_at_response[esp32_at_response_len] = '\0';
    }
}

uint8_t ESP32_AT_ParserContains(const char *text)
{
    return ((text != NULL) && (strstr(esp32_at_response, text) != NULL)) ? 1U : 0U;
}

uint8_t ESP32_AT_ParserGetIP(char *ip, uint16_t ip_size)
{
    const char *start;
    const char *end;
    uint16_t len;

    if ((ip == NULL) || (ip_size == 0U))
    {
        return 0U;
    }

    /* 典型响应格式：+CIFSR:STAIP,"192.168.1.10"。 */
    start = strstr(esp32_at_response, "+CIFSR:STAIP,\"");
    if (start == NULL)
    {
        return 0U;
    }
    start += strlen("+CIFSR:STAIP,\"");
    end = strchr(start, '"');
    if (end == NULL)
    {
        return 0U;
    }

    len = (uint16_t)(end - start);
    if (len >= ip_size)
    {
        return 0U;
    }

    memcpy(ip, start, len);
    ip[len] = '\0';
    return 1U;
}
