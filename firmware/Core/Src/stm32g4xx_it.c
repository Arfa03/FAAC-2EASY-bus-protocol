/**
 * stm32g4xx_it.c — handler minimi per lo sniffer BUS-2EASY.
 * Nessun handler ECU. La cattura DMA e' circolare e non genera interrupt.
 */

#include "main.h"
#include "stm32g4xx_it.h"

extern PCD_HandleTypeDef hpcd_USB_FS;

void NMI_Handler(void)
{
    /* Clock Security System: il quarzo e' caduto. Con HSI la temporizzazione
     * del bus non e' piu' affidabile: meglio fermarsi. */
    HAL_RCC_NMI_IRQHandler();
    while (1) {}
}

void HardFault_Handler(void)   { while (1) {} }
void MemManage_Handler(void)   { while (1) {} }
void BusFault_Handler(void)    { while (1) {} }
void UsageFault_Handler(void)  { while (1) {} }
void SVC_Handler(void)         { }
void DebugMon_Handler(void)    { }
void PendSV_Handler(void)      { }

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void USB_LP_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&hpcd_USB_FS);
}
