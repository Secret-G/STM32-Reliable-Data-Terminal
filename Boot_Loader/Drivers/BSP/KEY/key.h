#ifndef __KEY_H
#define __KEY_H

#include "stm32f4xx_hal.h"

typedef enum
{
    KEY_NONE = 0U,
    KEY0_PRESSED,
    KEY1_PRESSED,
    KEY2_PRESSED
} KeyValue_t;

/*
 * 原理图按键映射：
 * KEY0 -> PE4，KEY1 -> PE3，KEY2 -> PE2。
 * 三个按键均为低电平有效。
 */
KeyValue_t Key_Scan(void);

#endif
