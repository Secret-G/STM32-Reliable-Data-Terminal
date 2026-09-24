#include "app_update.h"
#include "boot_cache.h"
#include "boot_flag.h"
#include "boot_flash.h"
#include "wifi_ota_port.h"
#include "boot_crc.h"

typedef struct
{
    /* 保存一次完整升级会话的状态和接收进度。 */
    AppUpdate_StateTypeDef state;   /*升级状态*/
    Boot_StartInfoTypeDef image;    /*升级镜像信息*/
    Update_TargetTypeDef requested_target;/*请求的升级目标*/
    Boot_SlotTypeDef target_slot;/*下载的目标槽位*/
    uint32_t target_address;    /*目标地址*/
    uint32_t received_size;     /*接收到的大小*/
    uint32_t expected_sequence; /*期望的序列号*/
} AppUpdate_ContextTypeDef;

static AppUpdate_ContextTypeDef app_update_context;

/*初始化上下文*/
static void AppUpdate_ResetContext(void)
{
    app_update_context.state = APP_UPDATE_IDLE;

    app_update_context.image.target = UPDATE_NONE;
    app_update_context.image.image_size = 0U;
    app_update_context.image.image_crc = 0U;
    app_update_context.image.image_version = 0;

    app_update_context.requested_target = UPDATE_NONE;
    app_update_context.target_slot = BOOT_SLOT_NONE;
    app_update_context.target_address = 0U;
    app_update_context.received_size = 0U;
    app_update_context.expected_sequence = 0U;
}

static uint8_t AppUpdate_StartInfoMatches(const Boot_StartInfoTypeDef *start_info)
{
    if ((start_info->target == app_update_context.requested_target) &&
        (start_info->image_size == app_update_context.image.image_size) &&
        (start_info->image_crc == app_update_context.image.image_crc) &&
        (start_info->image_version == app_update_context.image.image_version))
    {
        return 1U;
    }

    return 0U;
}

/*选择非活动目标*/
static Update_TargetTypeDef AppUpdate_SelectInactiveTarget(
    const Boot_FlagInfoTypeDef *flag_info)
{
    if (flag_info == NULL)
    {
        return UPDATE_NONE;
    }

    if (flag_info->active_slot == BOOT_SLOT_A)
    {
        return UPDATE_APP_B;
    }

    if (flag_info->active_slot == BOOT_SLOT_B)
    {
        return UPDATE_APP_A;
    }

    if (flag_info->active_slot == BOOT_SLOT_NONE)
    {
        return UPDATE_APP_A;
    }

    return UPDATE_NONE;
}


/*擦除目标区域*/
static HAL_StatusTypeDef AppUpdate_PrepareTarget(Update_TargetTypeDef target, uint32_t *target_address)
{
    HAL_StatusTypeDef status;
    uint32_t first_sector;
    uint32_t second_sector;

    if (target_address == NULL)
    {
        return HAL_ERROR;
    }

    if (target == UPDATE_APP_A)
    {
        first_sector = FLASH_SECTOR_7;
        second_sector = FLASH_SECTOR_8;
        *target_address = APP_A_ADDR;
    }
    else if (target == UPDATE_APP_B)
    {
        first_sector = FLASH_SECTOR_9;
        second_sector = FLASH_SECTOR_10;
        *target_address = APP_B_ADDR;
    }
    else
    {
        return HAL_ERROR;
    }

    /* 每次只擦一个扇区，扇区之间及时喂狗。 */
    WifiOta_PortFeedWatchdog();
    status = Flash_EraseSector(first_sector);
    if (status != HAL_OK)
    {
        return status;
    }

    WifiOta_PortFeedWatchdog();

    status = Flash_EraseSector(second_sector);
    if (status != HAL_OK)
    {
        return status;
    }

    WifiOta_PortFeedWatchdog();
    /*初始化写入的目标地址*/
    BootCache_Init(*target_address);
    return HAL_OK;
}

void AppUpdate_Init(void)
{
    AppUpdate_ResetContext();
}

void AppUpdate_Abort(void)
{
    if ((app_update_context.state != APP_UPDATE_RECEIVING) &&
        (app_update_context.state != APP_UPDATE_ERROR))
    {
        return;
    }

    /* 半包镜像保持无效，只清理本次RAM会话。 */
    AppUpdate_ResetContext();
}
Boot_ResultTypeDef AppUpdate_HandleStart(const Boot_StartInfoTypeDef *start_info,uint32_t *response_value)
{
    Boot_StartInfoTypeDef selected_image;
    const Boot_FlagInfoTypeDef *flag_info;
    Update_TargetTypeDef selected_target;
    Boot_SlotTypeDef target_slot;
    HAL_StatusTypeDef status;
    uint32_t staging_capacity;
    uint32_t run_capacity;
    uint32_t current_version;
    uint32_t requested_version;
    uint32_t target_address;

    if ((start_info == NULL) || (response_value == NULL))
    {
        return BOOT_RESULT_FRAME_ERROR;
    }

    *response_value = (uint32_t)app_update_context.state;

    /*在这个函数后面才会设置接收状态，如果已经是接收状态，说明可能是掉包了*/
    if (app_update_context.state == APP_UPDATE_RECEIVING)
    {
        /* START的ACK丢失时，只重新应答，不重复初始化升级。 */
        if ((app_update_context.received_size == 0U) &&
            (app_update_context.expected_sequence == 0U) &&
            (AppUpdate_StartInfoMatches(start_info) != 0U))
        {
            *response_value = (uint32_t)app_update_context.image.target;
            return BOOT_RESULT_OK;
        }

        return BOOT_RESULT_STATE_ERROR;
    }

    /*idle状态直接返回*/
    if (app_update_context.state != APP_UPDATE_IDLE)
    {
        return BOOT_RESULT_STATE_ERROR;
    }

    /*计算a区大小*/
    staging_capacity = APP_A_END_ADDR - APP_A_ADDR + 1U;

    /*计算run区大小*/
    run_capacity = APP_RUN_END_ADDR - APP_RUN_ADDR + 1U;

    if ((start_info->image_size > staging_capacity) || (start_info->image_size > run_capacity))
    {
        *response_value = start_info->image_size;
        return BOOT_RESULT_IMAGE_TOO_LARGE;
    }

    /*喂狗*/
    WifiOta_PortFeedWatchdog();

    /*读取或初始化flag区*/
    status = Boot_FlagInit();
    if (status != HAL_OK)
    {
        *response_value = (uint32_t)status;
        return BOOT_RESULT_FLASH_ERROR;
    }

    /*读取当前flag信息*/
    flag_info = Boot_FlagGetInfo();

    /*选择非活动目标*/
    selected_target = AppUpdate_SelectInactiveTarget(flag_info);

    if (selected_target == UPDATE_NONE)
    {
        return BOOT_RESULT_STATE_ERROR;
    }

    if (selected_target == UPDATE_APP_A)
    {
        target_slot = BOOT_SLOT_A;
    }
    else
    {
        target_slot = BOOT_SLOT_B;
    }

    /*获取当前版本*/
    current_version = Boot_FlagGetActiveImageVersion();

    requested_version = (uint32_t)start_info->image_version;
    if ((requested_version == 0U) || ((current_version != 0U) && (requested_version < current_version)))
    {
        *response_value = current_version;
        return BOOT_RESULT_VERSION_ERROR;
    }

    /* 必须先将flag区的目标镜像失效，掉电后Bootloader才不会安装残缺固件。 */
    WifiOta_PortFeedWatchdog();
    status = Boot_FlagInvalidateImage(target_slot);
    if (status != HAL_OK)
    {
        app_update_context.state = APP_UPDATE_ERROR;
        *response_value = (uint32_t)status;
        return BOOT_RESULT_FLASH_ERROR;
    }

    /*擦除目标区域，并初始化写入的目标地址*/
    status = AppUpdate_PrepareTarget(selected_target, &target_address);
    if (status != HAL_OK)
    {
        app_update_context.state = APP_UPDATE_ERROR;
        *response_value = (uint32_t)status;
        return BOOT_RESULT_FLASH_ERROR;
    }

    /* 擦除成功后才保存会话并允许DATA写入。 */
    selected_image = *start_info;
    selected_image.target = selected_target;

    app_update_context.image = selected_image;

    app_update_context.requested_target = start_info->target;
    app_update_context.target_slot = target_slot;
    app_update_context.target_address = target_address;
    app_update_context.received_size = 0U;
    app_update_context.expected_sequence = 0U;

    app_update_context.state = APP_UPDATE_RECEIVING;

    *response_value = (uint32_t)app_update_context.image.target;
    return BOOT_RESULT_OK;
}

Boot_ResultTypeDef AppUpdate_HandleData(const Boot_DataInfoTypeDef *data_info, uint32_t *response_value)
{
    uint32_t remaining_size;
    HAL_StatusTypeDef status;

    if ((data_info == NULL) || (response_value == NULL))
    {
        return BOOT_RESULT_FRAME_ERROR;
    }

    *response_value = (uint32_t)app_update_context.state;

    if (app_update_context.state != APP_UPDATE_RECEIVING)
    {
        return BOOT_RESULT_STATE_ERROR;
    }

    if (data_info->sequence != app_update_context.expected_sequence)
    {
        /* 上一包ACK丢失时，允许上位机重发上一包。 */
        if ((app_update_context.expected_sequence > 0U) &&
        (data_info->sequence == (app_update_context.expected_sequence - 1U)))
        {
            *response_value = data_info->sequence;
            return BOOT_RESULT_OK;
        }

        *response_value = app_update_context.expected_sequence;
        return BOOT_RESULT_SEQUENCE_ERROR;
    }

    if (app_update_context.received_size > app_update_context.image.image_size)
    {
        app_update_context.state = APP_UPDATE_ERROR;
        *response_value = app_update_context.received_size;
        return BOOT_RESULT_IMAGE_SIZE_ERROR;
    }

    remaining_size = app_update_context.image.image_size - app_update_context.received_size;

    if ((uint32_t)data_info->data_len > remaining_size)
    {
        *response_value = remaining_size;
        return BOOT_RESULT_DATA_TOO_LARGE;
    }

    /*将数据写入缓存*/
    WifiOta_PortFeedWatchdog();
    status = Boot_WriteCache(data_info->data, data_info->data_len);
    if (status != HAL_OK)
    {
        app_update_context.state = APP_UPDATE_ERROR;
        *response_value = (uint32_t)status;
        return BOOT_RESULT_FLASH_ERROR;
    }

    /*更新接收进度和期望序列号*/
    app_update_context.received_size += data_info->data_len;
    app_update_context.expected_sequence++;

    *response_value = data_info->sequence;
    return BOOT_RESULT_OK;
}

Boot_ResultTypeDef AppUpdate_HandleEnd(uint32_t packet_count,uint32_t *response_value)
{
    HAL_StatusTypeDef status;
    uint16_t calculated_crc;

    if (response_value == NULL)
    {
        return BOOT_RESULT_FRAME_ERROR;
    }

    ///检查当前状态
    *response_value = (uint32_t)app_update_context.state;

    /*当接收状态已经是READY时，说明上一次END的ACK丢失了，只需要重新返回上一次成功应答即可*/
    if (app_update_context.state == APP_UPDATE_READY)
    {
        if ((packet_count == app_update_context.expected_sequence) &&
            (app_update_context.received_size ==
             app_update_context.image.image_size))
        {
            *response_value = app_update_context.expected_sequence;
            return BOOT_RESULT_OK;
        }

        *response_value = app_update_context.expected_sequence;
        return BOOT_RESULT_PACKET_COUNT_ERROR;
    }

    /*当前状态必须是接收中，才能处理END命令。*/
    if (app_update_context.state != APP_UPDATE_RECEIVING)
    {
        return BOOT_RESULT_STATE_ERROR;
    }

    /*检查DATA总包数是否与期望序号一致*/
    if (packet_count != app_update_context.expected_sequence)
    {
        *response_value = app_update_context.expected_sequence;
        return BOOT_RESULT_PACKET_COUNT_ERROR;
    }

    /* 检查接收的字节数是否与START声明的文件大小一致。*/
    if (app_update_context.received_size != app_update_context.image.image_size)
    {
        *response_value = app_update_context.received_size;
        return BOOT_RESULT_IMAGE_SIZE_ERROR;
    }

    //将缓存中的数据刷新到Flash中
    WifiOta_PortFeedWatchdog();
    status = Boot_FlushCache();
    if (status != HAL_OK)
    {
        app_update_context.state = APP_UPDATE_ERROR;
        *response_value = (uint32_t)status;
        return BOOT_RESULT_FLASH_ERROR;
    }

    calculated_crc = Boot_CRC16_Modbus((const uint8_t *)app_update_context.target_address, app_update_context.image.image_size);
    if (calculated_crc != app_update_context.image.image_crc)
    {
        app_update_context.state = APP_UPDATE_ERROR;
        *response_value = (uint32_t)calculated_crc;
        return BOOT_RESULT_IMAGE_CRC_ERROR;
    }

    WifiOta_PortFeedWatchdog();

    status = Boot_FlagSetPendingImage(app_update_context.target_slot,
                                      app_update_context.image.image_size,
                                      app_update_context.image.image_crc,
                                      app_update_context.image.image_version);
    if (status != HAL_OK)
    {       
        app_update_context.state = APP_UPDATE_ERROR;
        *response_value = (uint32_t)status;
        return BOOT_RESULT_FLASH_ERROR;
    }

    /* 所有包数和字节数正确，本次升级会话进入准备完成状态。 */
    app_update_context.state = APP_UPDATE_READY;
    *response_value = app_update_context.expected_sequence;
    
    return BOOT_RESULT_OK;
}

AppUpdate_StateTypeDef AppUpdate_GetState(void)
{
    return app_update_context.state;
}
