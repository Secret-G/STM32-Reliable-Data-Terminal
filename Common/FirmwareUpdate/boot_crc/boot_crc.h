#ifndef __BOOT_CRC_H
#define __BOOT_CRC_H

#include <stdint.h>

/*
 * Modbus CRC16
 *
 * 初始值：0xFFFF
 * 多项式：0xA001
 * 返回值：16位CRC
 */
uint16_t Boot_CRC16_Modbus(const uint8_t *data,uint32_t length);

#endif
