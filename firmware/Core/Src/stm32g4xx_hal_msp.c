/**
 * stm32g4xx_hal_msp.c — init di basso livello, solo ADC1 e OPAMP1.
 * TIM3, DMA e GPIO del bus sono configurati direttamente in b2e.c.
 */

#include "main.h"

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
    RCC_PeriphCLKInitTypeDef pclk = {0};

    if (hadc->Instance == ADC1) {
        pclk.PeriphClockSelection = RCC_PERIPHCLK_ADC12;
        pclk.Adc12ClockSelection  = RCC_ADC12CLKSOURCE_SYSCLK;
        if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) Error_Handler();

        __HAL_RCC_ADC12_CLK_ENABLE();
        /* Nessun GPIO: l'ingresso e' l'uscita interna di OPAMP1. */
    }
}

void HAL_ADC_MspDeInit(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1) __HAL_RCC_ADC12_CLK_DISABLE();
}

void HAL_OPAMP_MspInit(OPAMP_HandleTypeDef *hopamp)
{
    (void)hopamp;
    /* Il GPIO PA3 e il clock SYSCFG sono gia' predisposti in b2e_current_init(). */
}

void HAL_OPAMP_MspDeInit(OPAMP_HandleTypeDef *hopamp)
{
    (void)hopamp;
}
