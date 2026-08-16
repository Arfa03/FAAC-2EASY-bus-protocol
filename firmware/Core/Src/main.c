/**
 * main.c — sniffer BUS-2EASY su scheda Ignition Controller (STM32G474RCT6)
 *
 * Progetto ridotto all'osso: niente ECU, niente accensione, niente iniezione.
 * Tutte le uscite di potenza vengono forzate a riposo e non piu' toccate.
 *
 * Collegamenti:
 *   BUS+ --[100k]--+--[22k]-- GND        nodo centrale -> PC6  (conn. "DIN4")
 *   BUS- --[10R]-- GND                   nodo caldo    -> PA3  (conn. "AIN2")
 *
 * Console su USB CDC.
 */

#include "main.h"
#include "usb_device.h"
#include "b2e.h"
#include "b2e_master.h"

ADC_HandleTypeDef hadc1;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);

extern void b2e_app_run(void);

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_ADC1_Init();
    MX_USB_Device_Init();

    /* Sniffer: cattura di tensione su PC6 (TIM3_CH1 + DMA1_Ch3) */
    b2e_init();

    /* Canale corrente: PA3 -> OPAMP1 (PGA x4) -> ADC1 canale VOPAMP1 */
    b2e_current_init();
    HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
    HAL_ADC_Start(&hadc1);
    b2e_current_attach_adc(&hadc1);

    /* Master: configurato ma NON avviato. Si accende dalla console con 'M'. */
    b2em_init();

    b2e_app_run();          /* non ritorna */
}

/**
 * Clock: HSE 8 MHz (quarzo Y1) -> PLL /2 x85 /2 = 170 MHz.
 * HSI48 + CRS per l'USB. CSS attivo: se il quarzo cade si passa a HSI.
 */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef  osc = {0};
    RCC_ClkInitTypeDef  clk = {0};
    RCC_CRSInitTypeDef  crs = {0};

    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48 | RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.HSI48State     = RCC_HSI48_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = RCC_PLLM_DIV2;
    osc.PLL.PLLN       = 85;
    osc.PLL.PLLP       = RCC_PLLP_DIV2;
    osc.PLL.PLLQ       = RCC_PLLQ_DIV2;
    osc.PLL.PLLR       = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                       | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) Error_Handler();

    __HAL_RCC_CRS_CLK_ENABLE();
    crs.Prescaler             = RCC_CRS_SYNC_DIV1;
    crs.Source                = RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue           = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
    crs.ErrorLimitValue       = 34;
    crs.HSI48CalibrationValue = 32;
    HAL_RCCEx_CRSConfig(&crs);

    HAL_RCC_EnableCSS();
}

/**
 * ADC1 su un solo canale: l'uscita interna di OPAMP1.
 * Conversione continua, lettura a polling da b2e_current_sample().
 */
static void MX_ADC1_Init(void)
{
    ADC_ChannelConfTypeDef ch = {0};
    ADC_MultiModeTypeDef   mm = {0};

    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.GainCompensation      = 0;
    hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait      = DISABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIG_T1_TRGO;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    hadc1.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.OversamplingMode      = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();

    mm.Mode = ADC_MODE_INDEPENDENT;
    if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &mm) != HAL_OK) Error_Handler();

    ch.Channel      = ADC_CHANNEL_VOPAMP1;
    ch.Rank         = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_6CYCLES_5;
    ch.SingleDiff   = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    ch.Offset       = 0;
    if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) Error_Handler();
}

/**
 * GPIO: solo la messa in sicurezza delle uscite di potenza.
 * PC6 e PA3 li configurano b2e_init() / b2e_current_init(): non toccarli qui.
 */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* Bobina, iniettore, pompa: SPENTI e mai piu' toccati. */
    HAL_GPIO_WritePin(GPIOA, COIL_DRV_Pin | INJ_DRV_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PUMP_DRV_GPIO_Port, PUMP_DRV_Pin, GPIO_PIN_RESET);

    gpio.Pin   = COIL_DRV_Pin | INJ_DRV_Pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = PUMP_DRV_Pin;
    HAL_GPIO_Init(PUMP_DRV_GPIO_Port, &gpio);

    /* Driver IAC disabilitato */
    HAL_GPIO_WritePin(GPIOB, IAC_STEP_Pin | IAC_DIR_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(IAC_EN_GPIO_Port, IAC_EN_Pin, GPIO_PIN_SET);
    gpio.Pin = IAC_STEP_Pin | IAC_DIR_Pin | IAC_EN_Pin;
    HAL_GPIO_Init(GPIOB, &gpio);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file; (void)line;
}
#endif
