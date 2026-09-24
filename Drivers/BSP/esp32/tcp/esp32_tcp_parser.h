#ifndef ESP32_TCP_PARSER_H
#define ESP32_TCP_PARSER_H

#include "main.h"

/* TCP正文接收处理器类型；处理器在FreeRTOS任务上下文中执行。 */
typedef void (*ESP32_TCP_RxHandler_t)(const uint8_t *data, uint16_t len);

void ESP32_TCP_ParserInit(void);
/* 应用层显式注册正文处理器；传NULL可取消注册。 */
void ESP32_TCP_RegisterRxHandler(ESP32_TCP_RxHandler_t handler);
/* 返回1表示当前字节属于TCP正文，其他解析器不应再次处理该字节。 */
uint8_t ESP32_TCP_ParserFeed(uint8_t data);

#endif
