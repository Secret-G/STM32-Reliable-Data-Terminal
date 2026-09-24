#include "boot_confirm.h"

void Boot_ConfirmInit(void)
{
    /*Backup Domain访问控制位位于PWR外设，所以必须先打开PWR时钟*/
    __HAL_RCC_PWR_CLK_ENABLE();

    /*允许写入RTC Backup Domain*/
    HAL_PWR_EnableBkUpAccess();

    /*使能RTC接口，这里只使用Backup Register，不要求RTC进行计时*/
    __HAL_RCC_RTC_ENABLE();
}

/*检查试运行握手状态当前是否为空。*/
uint8_t Boot_ConfirmIsEmpty(void)
{
    if (RTC->BKP0R == BOOT_CONFIRM_EMPTY_MAGIC)
    {
        return 1U;
    }
    return 0U;
}

/*检查Bootloader是否要求APP执行试运行确认。*/
uint8_t Boot_ConfirmIsTrialRequested(void)
{
    if (RTC->BKP0R == BOOT_TRIAL_REQUEST_MAGIC)
    {
        return 1U;
    }
    return 0U;
}

/*检查APP是否已经写入运行成功确认。*/
uint8_t Boot_ConfirmIsConfirmed(void)
{
    if (RTC->BKP0R == BOOT_CONFIRM_MAGIC)
    {
        return 1U;
    }
    return 0U;
}

/*由Bootloader调用，写入试运行请求。*/
void Boot_ConfirmSetTrialRequest(void)
{
    RTC->BKP0R = BOOT_TRIAL_REQUEST_MAGIC;

    __DSB();
}

/*由候选APP调用，写入试运行成功确认。*/
void Boot_ConfirmSetConfirmed(void)
{
    RTC->BKP0R = BOOT_CONFIRM_MAGIC;

    __DSB();
}

/*清除整个试运行握手状态。*/
void Boot_ConfirmClear(void)
{
    RTC->BKP0R = BOOT_CONFIRM_EMPTY_MAGIC;

    __DSB();
}
