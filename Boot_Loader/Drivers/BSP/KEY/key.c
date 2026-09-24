#include "key.h"

#define KEY0_GPIO_PORT    GPIOE
#define KEY0_GPIO_PIN     GPIO_PIN_4
#define KEY1_GPIO_PORT    GPIOE
#define KEY1_GPIO_PIN     GPIO_PIN_3
#define KEY2_GPIO_PORT    GPIOE
#define KEY2_GPIO_PIN     GPIO_PIN_2

#define KEY_PRESSED_LEVEL GPIO_PIN_RESET

static uint8_t Key_AllReleased(void)
{
    return ((HAL_GPIO_ReadPin(KEY0_GPIO_PORT, KEY0_GPIO_PIN) == GPIO_PIN_SET) &&
            (HAL_GPIO_ReadPin(KEY1_GPIO_PORT, KEY1_GPIO_PIN) == GPIO_PIN_SET) &&
            (HAL_GPIO_ReadPin(KEY2_GPIO_PORT, KEY2_GPIO_PIN) == GPIO_PIN_SET));
}

KeyValue_t Key_Scan(void)
{
    static uint8_t key_released = 1U;

    if (Key_AllReleased() != 0U)
    {
        key_released = 1U;
        return KEY_NONE;
    }

    if (key_released == 0U)
    {
        return KEY_NONE;
    }

    HAL_Delay(20U);

    if (HAL_GPIO_ReadPin(KEY0_GPIO_PORT, KEY0_GPIO_PIN) == KEY_PRESSED_LEVEL)
    {
        key_released = 0U;
        return KEY0_PRESSED;
    }

    if (HAL_GPIO_ReadPin(KEY1_GPIO_PORT, KEY1_GPIO_PIN) == KEY_PRESSED_LEVEL)
    {
        key_released = 0U;
        return KEY1_PRESSED;
    }

    if (HAL_GPIO_ReadPin(KEY2_GPIO_PORT, KEY2_GPIO_PIN) == KEY_PRESSED_LEVEL)
    {
        key_released = 0U;
        return KEY2_PRESSED;
    }

    return KEY_NONE;
}
