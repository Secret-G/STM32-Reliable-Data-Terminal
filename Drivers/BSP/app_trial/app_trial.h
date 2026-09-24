#ifndef __APP_TRIAL_H
#define __APP_TRIAL_H

#include "stm32f4xx_hal.h"

/*
 * 候选APP正常运行多长时间后，
 * 才向Bootloader确认启动成功。
 */
#ifndef APP_TRIAL_CONFIRM_DELAY_MS
#define APP_TRIAL_CONFIRM_DELAY_MS    3000U
#endif

/*
 * 在APP完成时钟和外设初始化后调用一次。
 */
void App_TrialInit(void);

/*
 * 在APP主循环中持续调用。
 */
void App_TrialProcess(void);

#endif
