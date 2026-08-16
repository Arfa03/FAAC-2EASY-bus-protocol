/**
 * b2e.c — sniffer BUS-2EASY per STM32G474
 *
 * Periferiche usate (nessun conflitto con il firmware ECU originale):
 *   PC6        TIM3_CH1  input capture, fronte di discesa   (etichetta DIN4)
 *   TIM3       tick 1 us da 170 MHz (PSC = 169)
 *   DMA1_Ch3   TIM3_CH1 -> ring buffer                       (Ch1/Ch2 = ADC)
 *   OPAMP1     PA3 (VINP1) -> ADC1 canale VOPAMP1, PGA x4
 *   ADC1       campionamento corrente shunt                  (stadio 1b)
 *
 * Il buffer di cattura e' circolare e non genera interrupt: il consumatore
 * insegue l'indice di scrittura del DMA. Un frame contiene 71..87 fronti,
 * quindi 512 campioni coprono ~6 frame: il main loop ha 120 ms di margine.
 */

#include "b2e.h"
#include "stm32g4xx_hal.h"
#include <string.h>
#include <stdio.h>

/* ======================================================================== */
/* Buffer di cattura                                                        */
/* ======================================================================== */

#define B2E_CAP_LEN   512u              /* potenza di 2 */

static uint16_t          b2e_cap[B2E_CAP_LEN];
static TIM_HandleTypeDef htim_b2e;
static DMA_HandleTypeDef hdma_b2e;

static ADC_HandleTypeDef   *hadc_b2e = NULL;   /* opzionale, stadio 1b */
static OPAMP_HandleTypeDef  hopamp_b2e;
static volatile int32_t     b2e_i_ma100 = 0;   /* corrente shunt, mA x100 */

/* Guadagno del PGA: deve corrispondere a Init.PgaGain qui sotto. */
#define B2E_PGA_GAIN        4
/* Shunt in ohm. */
#define B2E_SHUNT_OHM      10

/* indice di scrittura corrente del DMA */
static inline uint32_t b2e_wr_index(void)
{
    return (uint32_t)(B2E_CAP_LEN - __HAL_DMA_GET_COUNTER(&hdma_b2e));
}

/* ======================================================================== */
/* Stato del decoder                                                        */
/* ======================================================================== */

static struct {
    uint32_t rd;
    uint16_t prev_cc;
    bool     have_prev;

    bool     in_frame;
    uint8_t  bits[B2E_FRAME_BITS];
    uint8_t  nbits;
    bool     pending_half;
} dec;

static b2e_stats_t stats;

/* ======================================================================== */
/* Init hardware                                                            */
/* ======================================================================== */

void b2e_init(void)
{
    GPIO_InitTypeDef        gpio = {0};
    TIM_IC_InitTypeDef      ic   = {0};
    TIM_ClockConfigTypeDef  clk  = {0};

    memset(&dec, 0, sizeof(dec));
    memset(&stats, 0, sizeof(stats));

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();

    /* --- PC6 come TIM3_CH1 -------------------------------------------- */
    /* IMPORTANTE: nessun pull. Il pull-up interno (~40 kOhm) falserebbe   */
    /* il partitore esterno 100k/22k.                                     */
    gpio.Pin       = GPIO_PIN_6;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* --- TIM3: tick 1 us ---------------------------------------------- */
    htim_b2e.Instance               = TIM3;
    htim_b2e.Init.Prescaler         = 169;          /* 170 MHz / 170 = 1 MHz */
    htim_b2e.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim_b2e.Init.Period            = 0xFFFF;
    htim_b2e.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim_b2e.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_IC_Init(&htim_b2e) != HAL_OK) { while (1) {} }

    clk.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    HAL_TIM_ConfigClockSource(&htim_b2e, &clk);

    ic.ICPolarity  = TIM_INPUTCHANNELPOLARITY_FALLING;
    ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
    ic.ICPrescaler = TIM_ICPSC_DIV1;
    ic.ICFilter    = 0;             /* segnale gia' pulito dal partitore */
    if (HAL_TIM_IC_ConfigChannel(&htim_b2e, &ic, TIM_CHANNEL_1) != HAL_OK) {
        while (1) {}
    }

    /* --- DMA1_Channel3, circolare, senza interrupt --------------------- */
    hdma_b2e.Instance                 = DMA1_Channel3;
    hdma_b2e.Init.Request             = DMA_REQUEST_TIM3_CH1;
    hdma_b2e.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_b2e.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_b2e.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_b2e.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_b2e.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_b2e.Init.Mode                = DMA_CIRCULAR;
    hdma_b2e.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&hdma_b2e) != HAL_OK) { while (1) {} }

    __HAL_LINKDMA(&htim_b2e, hdma[TIM_DMA_ID_CC1], hdma_b2e);

    if (HAL_TIM_IC_Start_DMA(&htim_b2e, TIM_CHANNEL_1,
                             (uint32_t *)b2e_cap, B2E_CAP_LEN) != HAL_OK) {
        while (1) {}
    }

    dec.rd = b2e_wr_index();
}

/*
 * Stadio 1b: canale corrente.
 *
 * Shunt da 10 Ohm sul ritorno del bus -> PA3 (AIN2) -> OPAMP1 in PGA x4,
 * uscita interna all'ADC.
 *     15 mA (riposo)  -> 150 mV -> 600 mV  ~745 LSB
 *      2 mA (impulso) ->  20 mV ->  80 mV   ~99 LSB
 *
 * NOTA HARDWARE: il condensatore di filtro su AIN2 forma con i 10 kOhm serie
 * una costante di tempo di ~1 ms e cancellerebbe un impulso da 100 us.
 * Portalo a 1 nF o rimuovilo.
 *
 * VINP1 = PA3 per OPAMP1 (verificato su stm32g4xx_hal_opamp.h:
 * IO0=PA1, IO1=PA3, IO2=PA7). PA1 e' l'NTC: non usarlo.
 */
void b2e_current_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    gpio.Pin  = GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    hopamp_b2e.Instance                    = OPAMP1;
    hopamp_b2e.Init.PowerMode              = OPAMP_POWERMODE_NORMALSPEED;
    hopamp_b2e.Init.Mode                   = OPAMP_PGA_MODE;
    hopamp_b2e.Init.NonInvertingInput      = OPAMP_NONINVERTINGINPUT_IO1;  /* VINP1 = PA3 */
    hopamp_b2e.Init.PgaGain                = OPAMP_PGA_GAIN_4_OR_MINUS_3;
    hopamp_b2e.Init.InternalOutput         = ENABLE;
    hopamp_b2e.Init.TimerControlledMuxmode = OPAMP_TIMERCONTROLLEDMUXMODE_DISABLE;
    hopamp_b2e.Init.UserTrimming           = OPAMP_TRIMMING_FACTORY;
    if (HAL_OPAMP_Init(&hopamp_b2e) != HAL_OK) { while (1) {} }
    HAL_OPAMP_Start(&hopamp_b2e);
}

void b2e_current_attach_adc(ADC_HandleTypeDef *h)
{
    hadc_b2e = h;
}

/* Converte un valore grezzo a 12 bit (riferito a 3.3 V) in mA x100. */
void b2e_current_set_raw(uint16_t raw)
{
    int32_t mv_adc   = (int32_t)(((uint32_t)raw * 3300u) / 4096u);
    int32_t mv_shunt = mv_adc / B2E_PGA_GAIN;
    b2e_i_ma100      = (mv_shunt * 100) / B2E_SHUNT_OHM;
}

void b2e_current_sample(void)
{
    if (hadc_b2e == NULL) return;
    if (HAL_ADC_PollForConversion(hadc_b2e, 2) != HAL_OK) return;
    b2e_current_set_raw((uint16_t)HAL_ADC_GetValue(hadc_b2e));
}

int32_t b2e_current_ma100(void) { return b2e_i_ma100; }

const b2e_stats_t *b2e_get_stats(void) { return &stats; }

/* ======================================================================== */
/* Decoder                                                                  */
/* ======================================================================== */

static void dec_reset_frame(void)
{
    dec.nbits = 0;
    dec.pending_half = false;
}

static void dec_abort(void)
{
    if (dec.nbits > 0) stats.frames_bad++;
    dec.in_frame = false;
    dec_reset_frame();
}

/* Chiude e valida il frame corrente. */
static bool dec_commit(b2e_frame_t *out)
{
    bool ok = (dec.nbits == B2E_FRAME_BITS);

    if (ok) {
        for (uint8_t i = 0; i < B2E_HEADER_BITS; i++) {
            if (dec.bits[i] != 1) { ok = false; break; }
        }
    }

    if (ok) {
        out->mask = 0;
        for (uint8_t i = 0; i < B2E_FRAME_BITS; i++) {
            if (dec.bits[i]) out->mask |= (uint64_t)1u << i;
        }
        out->seq = ++stats.frames_ok;
    } else if (dec.nbits > 0) {
        stats.frames_bad++;
    }

    dec_reset_frame();
    return ok;
}

bool b2e_poll(b2e_frame_t *out)
{
    uint32_t wr = b2e_wr_index();

    /* rilevamento sorpasso: se la distanza supera il buffer, abbiamo perso dati */
    uint32_t avail = (wr - dec.rd) & (B2E_CAP_LEN - 1u);
    if (avail > (B2E_CAP_LEN - 32u)) {
        stats.overrun++;
        dec.rd = wr;
        dec.have_prev = false;
        dec_abort();
        return false;
    }

    while (dec.rd != wr) {
        uint16_t cc = b2e_cap[dec.rd];
        dec.rd = (dec.rd + 1u) & (B2E_CAP_LEN - 1u);
        stats.captures++;

        if (!dec.have_prev) {
            dec.prev_cc = cc;
            dec.have_prev = true;
            continue;
        }

        uint16_t dt = (uint16_t)(cc - dec.prev_cc);   /* wrap gestito dal tipo */
        dec.prev_cc = cc;

        /* --- gap: chiude il frame precedente e apre il nuovo ------------- */
        if (dt > B2E_GAP_THRESHOLD_US) {
            bool got = false;
            if (dec.in_frame) got = dec_commit(out);
            dec.in_frame = true;
            dec_reset_frame();
            if (got) return true;
            continue;
        }

        if (!dec.in_frame) continue;

        /* --- intervallo implausibile ------------------------------------ */
        if (dt < B2E_MIN_VALID_US || dt > B2E_MAX_VALID_US) {
            dec_abort();
            continue;
        }

        /* --- decodifica -------------------------------------------------- */
        if (dt > B2E_BIT_THRESHOLD_US) {
            /* ~300 us: bit 0. Non puo' cadere a meta' di un bit 1. */
            if (dec.pending_half) { dec_abort(); continue; }
            if (dec.nbits < B2E_FRAME_BITS) dec.bits[dec.nbits] = 0;
            dec.nbits++;
        } else {
            /* ~150 us: semi-slot; due consecutivi formano un bit 1. */
            if (!dec.pending_half) {
                dec.pending_half = true;
            } else {
                dec.pending_half = false;
                if (dec.nbits < B2E_FRAME_BITS) dec.bits[dec.nbits] = 1;
                dec.nbits++;
            }
        }

        if (dec.nbits > B2E_FRAME_BITS) dec_abort();
    }

    return false;
}

/* ======================================================================== */
/* Censimento e appello                                                     */
/* ======================================================================== */

static struct { uint64_t acc; uint32_t count; } enroll;

void b2e_enroll_begin(void)
{
    enroll.acc   = ~(uint64_t)0;
    enroll.count = 0;
}

bool b2e_enroll_feed(const b2e_frame_t *f, b2e_nodes_t *n)
{
    enroll.acc &= f->mask;
    if (++enroll.count < B2E_ENROLL_FRAMES) return false;

    /* l'header e' sempre attivo: non e' un dispositivo */
    n->enrolled = enroll.acc & ~(uint64_t)0xFFu;
    n->valid    = true;
    memset(n->missing, 0, sizeof(n->missing));
    return true;
}

void b2e_nodes_update(b2e_nodes_t *n, const b2e_frame_t *f)
{
    n->last  = f->mask;
    n->fault = false;

    for (uint8_t s = B2E_HEADER_BITS; s < B2E_FRAME_BITS; s++) {
        uint64_t bit = (uint64_t)1u << s;
        if ((n->enrolled & bit) == 0) continue;

        /* Gli slot Stop NC sono a logica invertita: il dispositivo e'
         * presente in entrambi gli stati, quindi il livello del bit non
         * puo' servire da appello. */
        if (s == B2E_SLOT_STOP_NC_1 || s == B2E_SLOT_STOP_NC_2) continue;

        if (f->mask & bit) {
            n->missing[s] = 0;
        } else if (n->missing[s] < 0xFF) {
            n->missing[s]++;
        }

        if (n->missing[s] >= B2E_MISSING_LIMIT) n->fault = true;
    }

    n->intruder = f->mask & ~n->enrolled & ~(uint64_t)0xFFu;
}

/* ======================================================================== */
/* Formattazione                                                            */
/* ======================================================================== */

void b2e_format_frame(const b2e_frame_t *f, char *buf, uint32_t buflen)
{
    uint32_t k = 0;
    for (uint8_t i = 0; i < B2E_FRAME_BITS && k + 2 < buflen; i++) {
        if (i && (i % 8) == 0) buf[k++] = ' ';
        buf[k++] = ((f->mask >> i) & 1u) ? '1' : '0';
    }
    if (k < buflen) buf[k] = '\0';
    else if (buflen) buf[buflen - 1] = '\0';
}
