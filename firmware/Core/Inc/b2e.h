/**
 * b2e.h — sniffer BUS-2EASY per STM32G474 (scheda Ignition Controller)
 *
 * Modulo autonomo: configura da solo GPIO, TIM3, DMA e OPAMP/ADC.
 * Non richiede rigenerazione da CubeMX.
 *
 * STADIO 1  — solo ascolto. Nessun rischio, nessun pilotaggio del bus.
 * STADIO 1b — aggiunge il campionamento della corrente di shunt.
 *
 * Collegamenti (vedi banco-prova-g474.md):
 *   BUS+ --[100k]--+--[22k]-- GND      il nodo centrale va su PC6
 *                  '-------------> PC6   (etichetta DIN4 sul connettore)
 *   BUS- --[10R]-- GND                 il nodo caldo va su PA3 (AIN2)
 */

#ifndef B2E_H
#define B2E_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32g4xx_hal.h"

/* ---- costanti di protocollo ------------------------------------------- */

#define B2E_SLOT_US             300u
#define B2E_PULSE_LOW_US         99u
#define B2E_BIT_THRESHOLD_US    220u   /* > soglia => bit 0 ; <= soglia => bit 1 */
#define B2E_GAP_THRESHOLD_US    500u   /* intervallo che identifica il gap       */
#define B2E_FRAME_BITS           63u
#define B2E_HEADER_BITS           8u

#define B2E_SLOT_PHOTO_BASE      16u   /* fotocellule: slot = 16 + DIP  */
#define B2E_SLOT_CMD_BASE        32u   /* comandi:     slot = 32 + DIP  */
#define B2E_SLOT_STOP_NC_1       38u   /* logica INVERTITA              */
#define B2E_SLOT_STOP_NC_2       39u   /* logica INVERTITA              */

/* limiti di plausibilita' di un intervallo dentro il frame */
#define B2E_MIN_VALID_US        100u
#define B2E_MAX_VALID_US       2000u

/* ---- frame decodificato ------------------------------------------------ */

typedef struct {
    uint64_t mask;    /* bit i = 1  =>  slot i attivo (bit 0..62)            */
    uint32_t seq;     /* progressivo dei frame validi                        */
} b2e_frame_t;

/* ---- statistiche ------------------------------------------------------- */

typedef struct {
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t captures;      /* fronti di discesa consumati                   */
    uint32_t overrun;       /* buffer DMA sorpassato: il main loop e' lento  */
} b2e_stats_t;

/* ---- censimento dispositivi -------------------------------------------- */

#define B2E_ENROLL_FRAMES        50u   /* 1 secondo a 50 frame/s             */
#define B2E_MISSING_LIMIT         5u   /* ~100 ms prima di dichiarare guasto */

typedef struct {
    uint64_t enrolled;                  /* maschera censita                  */
    uint64_t last;                      /* ultimo frame ricevuto             */
    uint64_t intruder;                  /* slot attivi non censiti           */
    uint8_t  missing[B2E_FRAME_BITS];   /* frame consecutivi senza risposta  */
    bool     fault;                     /* errore di appello                 */
    bool     valid;                     /* censimento eseguito               */
} b2e_nodes_t;

/* ---- API --------------------------------------------------------------- */

/** Configura GPIO, TIM3, DMA e avvia la cattura. Chiamare dopo HAL_Init(). */
void b2e_init(void);

/**
 * Stadio 1b — canale corrente.
 * Configura PA3 (AIN2) come analogico e avvia OPAMP1 in PGA.
 * Richiede HAL_OPAMP_MODULE_ENABLED e il driver stm32g4xx_hal_opamp.c.
 */
void b2e_current_init(void);

/** Aggancia un ADC gia' inizializzato sull'uscita interna di OPAMP1. */
void b2e_current_attach_adc(ADC_HandleTypeDef *h);

/** Campiona la corrente. Da chiamare dal main loop. */
void b2e_current_sample(void);

/** Lettura grezza alternativa, se preferisci gestire tu l'ADC. */
void b2e_current_set_raw(uint16_t raw);

/**
 * Da chiamare in continuazione dal main loop.
 * Ritorna true e riempie *out ogni volta che un frame valido e' completo.
 * Richiamare finche' ritorna false per drenare il buffer.
 */
bool b2e_poll(b2e_frame_t *out);

/** Statistiche correnti. */
const b2e_stats_t *b2e_get_stats(void);

/** Ultimo valore di corrente campionato, in milliampere x100. */
int32_t b2e_current_ma100(void);

/* ---- censimento -------------------------------------------------------- */

/** Avvia il censimento. Le fotocellule devono essere allineate e libere. */
void b2e_enroll_begin(void);

/** Alimenta il censimento; ritorna true quando e' completo. */
bool b2e_enroll_feed(const b2e_frame_t *f, b2e_nodes_t *n);

/** Appello, da chiamare a ogni frame valido dopo il censimento. */
void b2e_nodes_update(b2e_nodes_t *n, const b2e_frame_t *f);

/* ---- interpretazione applicativa --------------------------------------- */

/** Fotocellula: true = fascio libero. Fail-safe: assenza => ostacolo. */
static inline bool b2e_photo_clear(uint64_t mask, uint8_t dip)
{
    return (mask >> (B2E_SLOT_PHOTO_BASE + dip)) & 1u;
}

/** Comando: true = attivo. E' un livello, non un impulso. */
static inline bool b2e_cmd_active(uint64_t mask, uint8_t dip)
{
    return (mask >> (B2E_SLOT_CMD_BASE + dip)) & 1u;
}

/** Stop NC: logica invertita, bit 0 = STOP attivo. */
static inline bool b2e_stop_nc_active(uint64_t mask, uint8_t slot)
{
    return !((mask >> slot) & 1u);
}

/** Rende il frame in forma leggibile: "11111111 00000000 ..." + '\0'. */
void b2e_format_frame(const b2e_frame_t *f, char *buf, uint32_t buflen);

#endif /* B2E_H */
