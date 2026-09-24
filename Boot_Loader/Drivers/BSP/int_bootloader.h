#ifndef __INT_BOOTLOADER_H
#define __INT_BOOTLOADER_H

#include "usart.h"
#include "boot_cache.h"
#include "boot_flag.h"
#define PACKET_DATA_SIZE 256U


void bootloader_init(void);

void Bootloader_Process(void);

void Boot_JumpToApp(void);

/*
 * 将Flag中记录的pending固件
 * 搬运到运行区并完成CRC校验。
 */
HAL_StatusTypeDef Boot_InstallPendingImage(void);

HAL_StatusTypeDef Boot_CopyToRun(uint32_t source_addr,uint32_t image_size);


/*按照active_slot检查Run区，用于正常启动。*/
uint8_t Boot_RunImageValid(void);

/*按照指定的A/B槽位信息检查Run区。*/
uint8_t Boot_RunImageValidForSlot(Boot_SlotTypeDef source_slot);

/*根据Flag状态准备运行区固件。*/
uint8_t Boot_PrepareRunImage(void);

#endif
