/**
 * main.h — sniffer BUS-2EASY, scheda Ignition Controller (STM32G474RCT6)
 */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

void Error_Handler(void);

/* ---- piedinatura della scheda ---------------------------------------- */
/* ATTENZIONE: le etichette DIN sul connettore sono in ordine inverso      */
/* rispetto ai numeri di pin. Il bus va su PC6, marcato "DIN4".            */

#define HALL_Pin            GPIO_PIN_0
#define HALL_GPIO_Port      GPIOC
#define MAP_Pin             GPIO_PIN_0
#define MAP_GPIO_Port       GPIOA
#define NTC_Pin             GPIO_PIN_1
#define NTC_GPIO_Port       GPIOA
#define AIN1_Pin            GPIO_PIN_2
#define AIN1_GPIO_Port      GPIOA
#define AIN2_Pin            GPIO_PIN_3      /* <-- shunt corrente bus      */
#define AIN2_GPIO_Port      GPIOA
#define AIN3_Pin            GPIO_PIN_4
#define AIN3_GPIO_Port      GPIOA
#define AIN4_Pin            GPIO_PIN_5
#define AIN4_GPIO_Port      GPIOA
#define IAC_STEP_Pin        GPIO_PIN_10
#define IAC_STEP_GPIO_Port  GPIOB
#define IAC_DIR_Pin         GPIO_PIN_11
#define IAC_DIR_GPIO_Port   GPIOB
#define IAC_EN_Pin          GPIO_PIN_12
#define IAC_EN_GPIO_Port    GPIOB
#define DIN4_Pin            GPIO_PIN_6      /* <-- segnale bus (TIM3_CH1)  */
#define DIN4_GPIO_Port      GPIOC
#define DIN3_Pin            GPIO_PIN_7
#define DIN3_GPIO_Port      GPIOC
#define DIN2_Pin            GPIO_PIN_8
#define DIN2_GPIO_Port      GPIOC
#define DIN1_Pin            GPIO_PIN_9
#define DIN1_GPIO_Port      GPIOC
#define COIL_DRV_Pin        GPIO_PIN_8
#define COIL_DRV_GPIO_Port  GPIOA
#define INJ_DRV_Pin         GPIO_PIN_9
#define INJ_DRV_GPIO_Port   GPIOA
#define PUMP_DRV_Pin        GPIO_PIN_10
#define PUMP_DRV_GPIO_Port  GPIOC

/* Alias leggibili per il bus */
#define B2E_RX_Pin          DIN4_Pin
#define B2E_RX_GPIO_Port    DIN4_GPIO_Port
#define B2E_ISENSE_Pin      AIN2_Pin
#define B2E_ISENSE_GPIO_Port AIN2_GPIO_Port

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
