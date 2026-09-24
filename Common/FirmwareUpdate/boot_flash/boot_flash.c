#include "boot_flash.h"

uint32_t Flash_GetSector(uint32_t addr)
{
    if (addr < FLASH_SECTOR_4_ADDR)
    {
        return INVALID_SECTOR;
    }

    if (addr < FLASH_SECTOR_5_ADDR)
    {
        return FLASH_SECTOR_4;
    }
    else if (addr < FLASH_SECTOR_6_ADDR)
    {
        return FLASH_SECTOR_5;
    }
    else if (addr < FLASH_SECTOR_7_ADDR)
    {
        return FLASH_SECTOR_6;
    }
    else if (addr < FLASH_SECTOR_8_ADDR)
    {
        return FLASH_SECTOR_7;
    }
    else if (addr < FLASH_SECTOR_9_ADDR)
    {
        return FLASH_SECTOR_8;
    }
    else if (addr < FLASH_SECTOR_10_ADDR)
    {
        return FLASH_SECTOR_9;
    }
    else if (addr < FLASH_SECTOR_11_ADDR)
    {
        return FLASH_SECTOR_10;
    }
    else if (addr <= FLASH_END_ADDR)
    {
        return FLASH_SECTOR_11;
    }

    return INVALID_SECTOR;
}

HAL_StatusTypeDef Flash_EraseSector(uint32_t sector)
{
    FLASH_EraseInitTypeDef erase_init = {0};
    HAL_StatusTypeDef status;
    uint32_t sector_error;

    /* 禁止擦除Bootloader所在的Sector 0～3 */
    if ((sector < FLASH_SECTOR_4) ||
        (sector > FLASH_SECTOR_11))
    {
        return HAL_ERROR;
    }

    status = HAL_FLASH_Unlock();
    if (status != HAL_OK)
    {
        return status;
    }

    erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector = sector;
    erase_init.NbSectors = 1U;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    status = HAL_FLASHEx_Erase(&erase_init, &sector_error);

    HAL_FLASH_Lock();

    return status;
}




HAL_StatusTypeDef Flash_Write(uint32_t addr,uint8_t *data,uint32_t len)
{
    uint32_t i;
    uint32_t temp;

    HAL_StatusTypeDef status;

    if((data == NULL) ||
       (len == 0U) ||
       ((addr & 0x03U)!=0U) ||
       ((len & 0x03U)!=0U))
    {
        return HAL_ERROR;
    }


    if((addr < FLAG_START_ADDR) ||
       (addr > FLASH_END_ADDR) ||
       (len > (FLASH_END_ADDR - addr + 1U)))
    {
        return HAL_ERROR;
    }


    status = HAL_FLASH_Unlock();

    if(status != HAL_OK)
    {
        return status;
    }


    for(i = 0; i < len; i += 4U)
    {

        temp = ((uint32_t)data[i])
             | ((uint32_t)data[i+1U]<<8)
             | ((uint32_t)data[i+2U]<<16)
             | ((uint32_t)data[i+3U]<<24);


        status = HAL_FLASH_Program(
                    FLASH_TYPEPROGRAM_WORD,
                    addr+i,
                    temp);


        if(status != HAL_OK)
        {
            break;
        }


        if(*(volatile uint32_t *)(addr+i)!=temp)
        {
            status = HAL_ERROR;
            break;
        }

    }


    HAL_FLASH_Lock();


    return status;
}

