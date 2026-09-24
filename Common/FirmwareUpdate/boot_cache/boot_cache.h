#ifndef __BOOT_CACHE_H
#define __BOOT_CACHE_H

#include "boot_flash.h"

/*
 * 初始化4字节缓存、APP写入地址和扇区擦除记录。
 */
void BootCache_Init(uint32_t start_addr);

/*
 * 接收任意长度数据，内部每攒够4字节写入一次Flash。
 */
HAL_StatusTypeDef Boot_WriteCache(const uint8_t *data, uint32_t len);

/*
 * 文件结束时，将缓存剩余的1～3字节补0xFF后写入。
 */
HAL_StatusTypeDef Boot_FlushCache(void);

#endif
