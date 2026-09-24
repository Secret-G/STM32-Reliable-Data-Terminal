#ifndef __BOOT_FLAG_H
#define __BOOT_FLAG_H

#include "boot_flash.h"

/*标志区固定识别值。*/
#define BOOT_FLAG_MAGIC  0x424F4F54U

/*
 * 标志结构版本。
 *
 * 将来如果Boot_FlagInfoTypeDef字段发生变化，
 * 可以增加这个版本号，避免新版程序错误解析旧结构。
 */
#define BOOT_FLAG_VERSION  3U

/*固件槽位类型固定使用uint32_t，保证写入Flash后的字段大小始终是4字节。*/
typedef uint32_t Boot_SlotTypeDef;

#define BOOT_SLOT_NONE  0U
#define BOOT_SLOT_A     1U
#define BOOT_SLOT_B     2U

/*固件安装状态。*/
typedef uint32_t Boot_InstallStateTypeDef;

/*当前没有待安装固件。*/
#define BOOT_INSTALL_IDLE        0U

/*A/B区中已有完整固件，等待搬运。*/
#define BOOT_INSTALL_PENDING     1U

/*正在从A/B区搬运到运行区，如果搬运过程中复位，下次启动发现此状态时可以重新搬运。*/
#define BOOT_INSTALLING          2U

/*新固件已经复制完成，等待获得第一次试运行机会。*/
#define BOOT_INSTALL_TRIAL_READY     3U

/*新固件已经开始试运行，等待APP确认运行成功。*/
#define BOOT_INSTALL_TRIAL_RUNNING   4U

/*试运行失败，正在把active_slot旧固件恢复到运行区。*/
#define BOOT_INSTALL_ROLLBACK        5U

/*安装或校验过程中发生错误。*/
#define BOOT_INSTALL_ERROR       6U

/*
 * 固件有效标记。
 *
 * 只有valid_mark等于这个固定值，
 * 才表示对应A/B区中的固件已经通过END校验。
 */
#define BOOT_IMAGE_VALID_MARK  0x5AA55AA5U

/*
 * 固件无效标记。
 *
 * 只要valid_mark不等于BOOT_IMAGE_VALID_MARK，
 * 都认为对应固件无效。
 */
#define BOOT_IMAGE_INVALID_MARK  0U

/*
 * 一份A/B备份固件的信息。
 *
 * A区和B区都使用这个结构，
 * 但分别占用标志结构中的不同成员。
 */
typedef struct
{
    /* 固件有效标记。
     * 等于BOOT_IMAGE_VALID_MARK：
     * 固件已经完整接收并通过CRC校验
     */
    uint32_t valid_mark;

    /*BIN文件的真实字节数。*/
    uint32_t image_size;

    /*整个BIN文件的CRC16，搬运前后都可以使用它重新校验。*/
    uint16_t image_crc;

    /*image_crc只有2字节，增加2字节reserved后，整个结构体大小可以保持4字节对齐。*/
    uint16_t reserved;

    /*固件版本号，使用4个字节分别保存：*/
    uint32_t image_version;

} Boot_ImageInfoTypeDef;


/*
 * Flash标志区保存的完整信息。
 *
 * 该结构将从FLAG_START_ADDR开始存放。
 */
typedef struct
{
    /*标志区固定识别值。必须等于BOOT_FLAG_MAGIC。*/
    uint32_t magic;

    /*标志结构版本。必须等于BOOT_FLAG_VERSION。*/
    uint32_t version;

    /*Flag事务序号。*/
    uint32_t sequence;

    /*A区固件信息。*/
    Boot_ImageInfoTypeDef app_a;

    /*B区固件信息。*/
    Boot_ImageInfoTypeDef app_b;

    /*当前运行区中的APP来自哪个备份区。BOOT_SLOT_NONE、A或者B。*/
    Boot_SlotTypeDef active_slot;

    /*下一次需要搬运到运行区的目标。END成功后设置为A或者B。*/
    Boot_SlotTypeDef pending_slot;

    /*当前安装状态。IDLE、PENDING、INSTALLING或者ERROR。*/
    Boot_InstallStateTypeDef install_state;

    /*保留字段，同时让整个结构保持4字节对齐。*/
    uint16_t reserved;

    /*整个标志结构的CRC16。*/
    uint16_t flag_crc;

} Boot_FlagInfoTypeDef;

/*初始化标志模块。*/
HAL_StatusTypeDef Boot_FlagInit(void);

/*根据标志结构内容计算CRC16。*/
uint16_t Boot_FlagCalculateCRC(const Boot_FlagInfoTypeDef *flag_info);

/*重新计算CRC并保存到flag_crc字段。*/
void Boot_FlagUpdateCRC(Boot_FlagInfoTypeDef *flag_info);

/*检查结构中保存的CRC是否正确。*/
uint8_t Boot_FlagCRCValid(const Boot_FlagInfoTypeDef *flag_info);

/*将Flash中的完整Flag结构复制到RAM结构体中。*/
void Boot_FlagRead(Boot_FlagInfoTypeDef *flag_info);

/*检查一份标志结构是否可以被使用。*/
uint8_t Boot_FlagInfoValid(const Boot_FlagInfoTypeDef *flag_info);

/*读取标志区起始位置保存的magic。*/
uint32_t Boot_FlagReadMagic(void);

/*检查标志区magic是否正确。*/
uint8_t Boot_FlagMagicValid(void);

/*在RAM中生成一份默认标志信息。*/
void Boot_FlagSetDefault(Boot_FlagInfoTypeDef *flag_info);

/*将RAM中的完整标志结构写入Flash标志区*/
HAL_StatusTypeDef Boot_FlagWrite(Boot_FlagInfoTypeDef *flag_info);

/*获取当前RAM标志信息*/
const Boot_FlagInfoTypeDef *Boot_FlagGetInfo(void);

/*将指定A/B槽位标记为无效，必须在擦除对应固件区之前调用， 防止升级中途掉电后将残缺固件认为有效。*/
HAL_StatusTypeDef Boot_FlagInvalidateImage(Boot_SlotTypeDef slot);

/*************************修改状态**************************/

/*APP标记为接收完成并通过CRC校验的固件*/
HAL_StatusTypeDef Boot_FlagSetPendingImage(Boot_SlotTypeDef slot,uint32_t image_size,uint16_t image_crc, uint32_t image_version);

/*APP标记为开始安装。*/
HAL_StatusTypeDef Boot_FlagBeginInstall(void);

/*APP标记为已经搬运到了运行区，等待试运行。*/
HAL_StatusTypeDef Boot_FlagFinishInstall(void);

/*APP标记为正在试运行，调用成功后，Bootloader才可以跳转到Run区。*/
HAL_StatusTypeDef Boot_FlagBeginTrial(void);

/*APP标记为试运行成功，将pending_slot正式升级为active_slot。*/
HAL_StatusTypeDef Boot_FlagConfirmTrial(void);

/*第一次安装的候选APP试运行失败，当前没有旧版本可以回滚，放弃本次候选固件。*/
HAL_StatusTypeDef Boot_FlagAbortTrial(void);

/*APP标记为安装失败*/
HAL_StatusTypeDef Boot_FlagSetInstallError(void);

/*标记开始回滚，调用成功后，Bootloader可以开始把active_slot恢复到Run区。*/
HAL_StatusTypeDef Boot_FlagBeginRollback(void);

/*标记回滚完成，清除失败的候选固件信息，恢复到IDLE状态。*/
HAL_StatusTypeDef Boot_FlagFinishRollback(void);

/*
 * 获取当前活动固件版本，
 * 返回0：当前没有有效的活动固件，或者活动固件没有版本信息。
 * 返回非0：当前活动固件的32位版本号。
 */
uint32_t Boot_FlagGetActiveImageVersion(void);

#endif
