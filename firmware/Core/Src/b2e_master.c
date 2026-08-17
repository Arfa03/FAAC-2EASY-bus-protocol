/**
 * b2e_master.c — master BUS-2EASY, rilevamento in TENSIONE
 *
 * Il master genera il reticolo degli slot e alimenta la linea. Gli slave
 * abbassano la tensione da soli dentro la propria finestra: non c'e' nulla da
 * rigenerare e non serve misurare corrente.
 *
 * Struttura, a MEZZI SLOT da 150 us:
 *   mezzo slot PARI    -> il master emette l'impulso (clock del bus)
 *   mezzo slot DISPARI -> il master TACE. Se la linea scende comunque, e' uno
 *                         slave che risponde: bit = 1.
 *                         Eccezione: nei primi 8 slot (header) il master emette
 *                         anche nel mezzo slot dispari.
 *
 * Uno slot = 300 us = due mezzi slot. Un frame = 64 slot + gap da 1.05 ms.
 *
 *   TIM1  periodo 150 us
 *     UPDATE (t=0)        -> decide se emettere e, se si', accende il PNP
 *     CC1    (t=PULSE_US) -> chiude l'impulso
 *     CC2    (t=sample)   -> campiona il livello della linea
 *
 * PA9 pilota, tramite un MOSFET invertente, la base di un PNP high-side:
 *   PA9 alto  -> PNP acceso  -> linea alta
 *   PA9 basso -> PNP spento  -> linea rilasciata, scende
 *
 * PC6 legge la linea tramite partitore, A VALLE della resistenza serie del
 * collettore: e' li' che il bus e' cedevole e gli slave possono abbassarlo.
 */

#include "b2e_master.h"
#include "main.h"
#include <string.h>

#define B2EM_TX_PORT     GPIOA
#define B2EM_TX_PIN      GPIO_PIN_9
#define B2EM_RX_PORT     GPIOC
#define B2EM_RX_PIN      GPIO_PIN_6

static TIM_HandleTypeDef htim_m;
static b2em_state_t      st;
static uint16_t          sample_us = B2EM_SAMPLE_US_DEFAULT;

typedef enum { PH_DATA, PH_GAP } phase_t;

static volatile phase_t  phase;
static volatile uint16_t half_idx;
static volatile uint16_t gap_left;
static volatile uint64_t rx_mask;
static volatile uint16_t raw_buf[B2EM_SLOTS];   /* 0 = alto, 1 = basso */
static volatile uint16_t cur_half;

/* ======================================================================= */

static inline void bus_release(void)      { B2EM_TX_PORT->BSRR = B2EM_TX_PIN; }
static inline void bus_low(void)  { B2EM_TX_PORT->BRR  = B2EM_TX_PIN; }

static inline bool bus_is_low(void)
{
    /* il partitore non inverte: bus basso = pin basso */
    return (B2EM_RX_PORT->IDR & B2EM_RX_PIN) == 0u;
}

/* ======================================================================= */

void b2em_init(void)
{
    GPIO_InitTypeDef        gpio = {0};
    TIM_OC_InitTypeDef      oc   = {0};
    TIM_ClockConfigTypeDef  clk  = {0};

    memset(&st, 0, sizeof(st));

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE();

    /* PA9: comando del PNP high-side tramite MOSFET invertente */
    bus_release();
    gpio.Pin   = B2EM_TX_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(B2EM_TX_PORT, &gpio);

    /* PC6 resta configurato da b2e_init() come TIM3_CH1 in alternate
     * function: lo si puo' comunque leggere via IDR. Nessuna modifica. */

    htim_m.Instance               = TIM1;
    htim_m.Init.Prescaler         = 169;              /* tick 1 us */
    htim_m.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim_m.Init.Period            = B2EM_HALF_US - 1; /* 150 us */
    htim_m.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim_m.Init.RepetitionCounter = 0;
    htim_m.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim_m) != HAL_OK) Error_Handler();

    clk.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim_m, &clk) != HAL_OK) Error_Handler();

    if (HAL_TIM_OC_Init(&htim_m) != HAL_OK) Error_Handler();

    oc.OCMode       = TIM_OCMODE_TIMING;     /* nessuna uscita fisica */
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;

    oc.Pulse = B2EM_PULSE_US;                /* CC1: fine impulso a 99 us */
    if (HAL_TIM_OC_ConfigChannel(&htim_m, &oc, TIM_CHANNEL_1) != HAL_OK)
        Error_Handler();

    oc.Pulse = sample_us;                    /* CC2: istante di lettura */
    if (HAL_TIM_OC_ConfigChannel(&htim_m, &oc, TIM_CHANNEL_2) != HAL_OK)
        Error_Handler();

    HAL_NVIC_SetPriority(TIM1_UP_TIM16_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_TIM16_IRQn);
    HAL_NVIC_SetPriority(TIM1_CC_IRQn, 0, 1);
    HAL_NVIC_EnableIRQ(TIM1_CC_IRQn);
}

void b2em_start(void)
{
    phase      = PH_DATA;
    half_idx   = 0;
    rx_mask    = 0;
    st.frames  = 0;
    st.running = true;

    __HAL_TIM_SET_COUNTER(&htim_m, 0);
    __HAL_TIM_CLEAR_FLAG(&htim_m, TIM_FLAG_UPDATE | TIM_FLAG_CC1 | TIM_FLAG_CC2);
    __HAL_TIM_ENABLE_IT(&htim_m, TIM_IT_UPDATE | TIM_IT_CC1 | TIM_IT_CC2);
    __HAL_TIM_ENABLE(&htim_m);
}

void b2em_stop(void)
{
    __HAL_TIM_DISABLE_IT(&htim_m, TIM_IT_UPDATE | TIM_IT_CC1 | TIM_IT_CC2);
    __HAL_TIM_DISABLE(&htim_m);
    bus_release();
    st.running = false;
}

void b2em_blink_test(uint32_t cycles)
{
    b2em_stop();
    for (uint32_t i = 0; i < cycles; i++) {
        bus_low();      HAL_Delay(500);
        bus_release();  HAL_Delay(500);
    }
}

/* ======================================================================= */
/* Compare: CC1 chiude l'impulso, CC2 campiona la linea.                   */
/* ======================================================================= */

void TIM1_CC_IRQHandler(void)
{
    if (__HAL_TIM_GET_FLAG(&htim_m, TIM_FLAG_CC1) != RESET) {
        __HAL_TIM_CLEAR_FLAG(&htim_m, TIM_FLAG_CC1);
        bus_release();
    }

    if (__HAL_TIM_GET_FLAG(&htim_m, TIM_FLAG_CC2) != RESET) {
        __HAL_TIM_CLEAR_FLAG(&htim_m, TIM_FLAG_CC2);

        /* Si legge solo nei mezzi slot DISPARI, dove il master tace.
         * Se la linea e' bassa, e' uno slave che risponde. */
        if (st.running && phase == PH_DATA && (cur_half & 1u)) {
            uint16_t slot = cur_half >> 1;
            if (slot < B2EM_SLOTS) {
                bool low = bus_is_low();
                raw_buf[slot] = low ? 1u : 0u;
                if (low && slot >= B2E_HEADER_BITS)
                    rx_mask |= (uint64_t)1u << slot;
            }
        }
    }
}

/* ======================================================================= */
/* Update: inizio di ogni mezzo slot.                                      */
/* ======================================================================= */

void TIM1_UP_TIM16_IRQHandler(void)
{
    if (__HAL_TIM_GET_FLAG(&htim_m, TIM_FLAG_UPDATE) == RESET) return;
    __HAL_TIM_CLEAR_FLAG(&htim_m, TIM_FLAG_UPDATE);
    if (!st.running) return;

    if (phase == PH_GAP) {
        if (--gap_left == 0) {
            phase    = PH_DATA;
            half_idx = 0;
            st.frames++;
        } else {
            return;
        }
    }
    cur_half = half_idx;
    uint16_t slot = half_idx >> 1;

    if ((half_idx & 1u) == 0) {
        /* mezzo slot PARI: clock del bus, impulso sempre presente */
        bus_low();
    } else {
        /* mezzo slot DISPARI: il master tace.
         * Eccezione: l'header deve essere emesso, sono i primi 8 slot. */
        if (slot < B2E_HEADER_BITS) bus_low();
    }

    if (++half_idx >= (B2EM_SLOTS * 2u)) {
        phase    = PH_GAP;
        gap_left =  6u;
        st.mask  = rx_mask;
        rx_mask  = 0;
    }
}

/* ======================================================================= */


/* ======================================================================= */
/* Censimento e appello                                                     */
/*                                                                          */
/* L'iscrizione BUS-2EASY e' un censimento puramente lato master: gli slave  */
/* rispondono comunque, e' il master che decide quali contare. Memorizzare   */
/* significa salvare la maschera degli slot attivi.                          */
/* ======================================================================= */

#define B2EM_ENROLL_FRAMES   50u   /* 1 secondo a 50 frame/s               */
#define B2EM_MISSING_LIMIT    5u   /* ~100 ms prima di dichiarare guasto   */

static uint64_t enrolled_mask;
static bool     enrolled_ok;
static bool     enrolling;
static uint64_t enroll_acc;
static uint32_t enroll_count;
static uint8_t  missing[B2EM_SLOTS];
static bool     appello_fault;
static uint64_t intruder_mask;
static uint32_t last_frame_seen;

void b2em_enroll_begin(void)
{
    enroll_acc   = ~(uint64_t)0;
    enroll_count = 0;
    enrolling    = true;
    enrolled_ok  = false;
}

bool     b2em_enrolled(void)      { return enrolled_ok;   }
uint64_t b2em_enrolled_mask(void) { return enrolled_mask; }
bool     b2em_fault(void)         { return appello_fault; }
uint64_t b2em_intruder(void)      { return intruder_mask; }

static void b2em_census(uint64_t mask)
{
    if (enrolling) {
        enroll_acc &= mask;
        if (++enroll_count >= B2EM_ENROLL_FRAMES) {
            enrolled_mask = enroll_acc & ~(uint64_t)0xFFu;  /* header escluso */
            enrolled_ok   = true;
            enrolling     = false;
            for (uint16_t i = 0; i < B2EM_SLOTS; i++) missing[i] = 0;
        }
        return;
    }

    if (!enrolled_ok) return;

    appello_fault = false;

    for (uint16_t s = B2E_HEADER_BITS; s < B2EM_SLOTS; s++) {
        uint64_t bit = (uint64_t)1u << s;
        if ((enrolled_mask & bit) == 0) continue;

        /* Gli slot Stop NC sono a logica invertita: il dispositivo e' presente
         * in entrambi gli stati, quindi il livello del bit non puo' servire da
         * appello. Non e' un problema: l'assenza produce comunque lo stato
         * sicuro. */
        if (s == B2E_SLOT_STOP_NC_1 || s == B2E_SLOT_STOP_NC_2) continue;

        if (mask & bit)                 missing[s] = 0;
        else if (missing[s] < 0xFF)     missing[s]++;

        if (missing[s] >= B2EM_MISSING_LIMIT) appello_fault = true;
    }

    intruder_mask = mask & ~enrolled_mask & 0x00000000FFFFFF00ull;
}

/*
 * Consenso al movimento. Fail-safe: ogni condizione dubbia nega il consenso.
 *   master fermo, censimento non fatto, errore di appello, slot non censiti
 *   attivi, fotocellula richiesta non libera, Stop NC a 0 -> nessun consenso.
 */
bool b2em_motion_allowed(uint64_t photo_mask)
{
    if (!st.running)   return false;
    if (!enrolled_ok)  return false;
    if (appello_fault) return false;
    if (intruder_mask) return false;

    uint64_t m = st.mask;

    if ((m & photo_mask) != photo_mask) return false;

    /* logica invertita: bit 0 = STOP attivo */
    if ((enrolled_mask >> B2E_SLOT_STOP_NC_1) & 1u)
        if (!((m >> B2E_SLOT_STOP_NC_1) & 1u)) return false;
    if ((enrolled_mask >> B2E_SLOT_STOP_NC_2) & 1u)
        if (!((m >> B2E_SLOT_STOP_NC_2) & 1u)) return false;

    return true;
}

void b2em_poll(void)
{
    if (!st.running) return;
    for (uint16_t i = 0; i < B2EM_SLOTS; i++) st.raw[i] = raw_buf[i];

    /* elabora il censimento una sola volta per frame */
    if (st.frames != last_frame_seen) {
        last_frame_seen = st.frames;
        b2em_census(st.mask);
    }
}

/* Reliquia dell'approccio a misura di corrente: non fa nulla. */
void b2em_calibrate(void)
{
    st.baseline  = 0;
    st.threshold = 0;
}

void b2em_set_margin(uint16_t counts) { (void)counts; }

/** Sposta l'istante di lettura dentro il mezzo slot dispari. */
void b2em_set_sample_us(uint16_t us)
{
    if (us < 5u || us > (B2EM_HALF_US - 5u)) return;
    sample_us = us;
    __HAL_TIM_SET_COMPARE(&htim_m, TIM_CHANNEL_2, sample_us);
}

const b2em_state_t *b2em_get_state(void) { return &st; }
