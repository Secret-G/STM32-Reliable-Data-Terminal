#ifndef __BOOT_CONFIRM_H
#define __BOOT_CONFIRM_H

#include "stm32f4xx_hal.h"

/*
 * Backup Register为空，
 * 表示当前没有试运行握手任务。
 */
#define BOOT_CONFIRM_EMPTY_MAGIC          0x00000000UL

/*
 * Bootloader写入该值，
 * 通知候选APP本次启动属于试运行。
 * 0x54='T'，0x52='R'，0x49='I'，0x41='A'。
 */
#define BOOT_TRIAL_REQUEST_MAGIC          0x54524941UL

/*
 * 候选APP自检成功后写入该值。
 * 0x43='C'，0x4F='O'，0x4E='N'，0x46='F'。
 */
#define BOOT_CONFIRM_MAGIC                0x434F4E46UL
/*
 * 初始化Backup Register访问能力。
 *
 * 注意：该函数不能清除确认口令，
 * 因为Bootloader启动后还需要读取APP留下的确认结果。
 */
void Boot_ConfirmInit(void);

/*清除整个试运行握手状态。*/
void Boot_ConfirmClear(void);

/*检查握手状态当前是否为空。*/
uint8_t Boot_ConfirmIsEmpty(void);

/*由Bootloader调用，写入试运行请求。*/
void Boot_ConfirmSetTrialRequest(void);

/*检查Bootloader是否要求APP执行试运行确认。*/
uint8_t Boot_ConfirmIsTrialRequested(void);

/*由候选APP调用，写入试运行成功确认。*/
void Boot_ConfirmSetConfirmed(void);
/*检查APP是否已经写入运行成功确认。*/
uint8_t Boot_ConfirmIsConfirmed(void);


#endif
