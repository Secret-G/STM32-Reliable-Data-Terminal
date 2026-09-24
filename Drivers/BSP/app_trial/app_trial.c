#include "app_trial.h"
#include "boot_confirm.h"

#include "wifi_ota_port.h"

#include <stdio.h>

/*
 * 等于1表示本次启动属于候选APP试运行。
 *
 * 使用static后，该变量只能在当前文件内部访问，
 * main.c不需要知道它的存在。
 */
static uint8_t app_trial_pending = 0U;

/*
 * 保存候选APP开始试运行的时间。
 */
static uint32_t app_trial_start_tick = 0U;


void App_TrialInit(void)
{
    /*
     * 初始化RTC Backup Register访问能力。
     */
    Boot_ConfirmInit();

    /*
     * 只有Bootloader写入了TRIAL_REQUEST，
     * 当前APP才需要完成试运行确认。
     */
    if (Boot_ConfirmIsTrialRequested() != 0U)
    {
        app_trial_pending = 1U;
        app_trial_start_tick = HAL_GetTick();

        printf("APP_TRIAL_START\r\n");
    }
    else
    {
        /*
         * 普通启动不需要确认，
         * 也不能主动执行软件复位。
         */
        app_trial_pending = 0U;
        app_trial_start_tick = 0U;

        printf("APP_NORMAL_START\r\n");
    }
}

void App_TrialProcess(void)
{
    /*
     * 普通启动不执行任何处理。
     */
    if (app_trial_pending == 0U)
    {
		WifiOta_PortFeedWatchdog();
        return;
    }

    /*
     * 候选APP尚未正常运行到规定时间。
     */
    if ((HAL_GetTick() - app_trial_start_tick) < APP_TRIAL_CONFIRM_DELAY_MS)
    {
		WifiOta_PortFeedWatchdog();
        return;
    }

    /*
     * APP已经正常运行到确认时间，
     * 写入CONFIRMED口令。
     */
    Boot_ConfirmSetConfirmed();

    /*
     * 回读确认，确保Backup Register写入成功。
     */
    if (Boot_ConfirmIsConfirmed() == 0U)
    {
        printf("APP_TRIAL_CONFIRM_ERROR\r\n");
        return;
    }

    /*
     * 防止在软件复位前重复执行确认。
     */
    app_trial_pending = 0U;

    printf("APP_TRIAL_CONFIRM_OK\r\n");

    /*
     * 仅用于等待调试日志发送完成。
     */
    HAL_Delay(100U);

    /*
     * 软件复位，重新进入Bootloader，
     * 由Bootloader正式确认候选版本。
     */
    NVIC_SystemReset();
}
