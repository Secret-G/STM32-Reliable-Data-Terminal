#ifndef ESP32_AT_PARSER_H
#define ESP32_AT_PARSER_H

#include "main.h"

void ESP32_AT_ParserInit(void);
void ESP32_AT_ParserClear(void);
void ESP32_AT_ParserFeed(uint8_t data);
uint8_t ESP32_AT_ParserContains(const char *text);
uint8_t ESP32_AT_ParserGetIP(char *ip, uint16_t ip_size);

#endif
