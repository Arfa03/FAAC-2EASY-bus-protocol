/**
 * b2e_master.h — master BUS-2EASY su STM32G474
 *
 * Genera il reticolo dei 64 slot, campiona la corrente di ciascuno slot e
 * rigenera il bit di risposta quando uno slave assorbe.
 *
 * ATTENZIONE: questo modulo PILOTA il bus. Non abilitarlo finche' lo sniffer
 * non ha girato un'ora senza errori.
 *
 * Hardware (vedi README-master.md):
 *   13.6V --[100R 2W]--+-------------------- BUS+
 *                      |
 *                 drain IRLZ44N  (uscita "Iniettore", PA9 = TIM1_CH2)
 *                      |
 *                     GND
 *   slave BUS- --[10R]-- GND, nodo caldo su PA3 (OPAMP1 -> ADC1)
 */

#ifndef B2E_MASTER_H
#define B2E_MASTER_H

#include <stdint.h>
#include <stdbool.h>
#include "b2e.h"

/* ---- geometria del frame ---------------------------------------------- */

#define B2EM_HALF_US          150u   /* mezzo slot                          */
#define B2EM_PULSE_US          99u   /* impulso basso                       */
#define B2EM_SLOTS             64u   /* slot per frame                      */
#define B2EM_GAP_US          1050u   /* gap interframe, linea alta          */

/* Finestra di campionamento della corrente, in us dall'inizio del mezzo
 * slot pari. Il master originale rigenera l'impulso a 150-157 us dall'inizio
 * dello slot, quindi lo slave deve assorbire prima. 125 us e' il punto di
 * partenza; tarabile a runtime. */
#define B2EM_SAMPLE_US_DEFAULT  125u

/* ---- stato ------------------------------------------------------------- */

typedef struct {
    uint64_t mask;             /* slot in cui e' stata rilevata una risposta */
    uint16_t raw[B2EM_SLOTS];  /* campione ADC grezzo per ogni slot          */
    uint16_t baseline;         /* livello di riposo, medio                   */
    uint16_t threshold;        /* baseline + margine                         */
    uint32_t frames;
    bool     running;
} b2em_state_t;

/* ---- API --------------------------------------------------------------- */

/** Configura TIM1, DMA e ADC. Non accende ancora il bus. */
void b2em_init(void);

/** Accende la linea e comincia a generare i frame. */
void b2em_start(void);

/** Spegne il pull-down e ferma la generazione (linea alta, poi ferma). */
void b2em_stop(void);

/** Da chiamare dal main loop: elabora i campioni dell'ultimo frame. */
void b2em_poll(void);

/** Misura il livello di riposo e imposta la soglia. Bus acceso, nodi fermi. */
void b2em_calibrate(void);

/** Margine sopra il livello di riposo, in conteggi ADC. */
void b2em_set_margin(uint16_t counts);

/** Sposta la finestra di campionamento, in us dall'inizio del mezzo slot. */
void b2em_set_sample_us(uint16_t us);

const b2em_state_t *b2em_get_state(void);

/* ---- censimento e appello (lato master) ------------------------------- */

/** Avvia il censimento: accumula 50 frame e salva gli slot sempre attivi.
 *  Le fotocellule devono essere allineate e libere durante la procedura. */
void b2em_enroll_begin(void);

/** true quando il censimento e' stato completato. */
bool b2em_enrolled(void);

/** Maschera dei dispositivi censiti. */
uint64_t b2em_enrolled_mask(void);

/** Errore di appello: un nodo censito non risponde da troppi frame. */
bool b2em_fault(void);

/** Slot attivi che non risultano censiti. */
uint64_t b2em_intruder(void);

/** Consenso al movimento. Nega in ogni condizione dubbia.
 *  photo_mask: slot fotocellula che devono avere il fascio libero. */
bool b2em_motion_allowed(uint64_t photo_mask);

/** Diagnostica: PA9 come GPIO, onda quadra a 1 Hz. Verifica il percorso HW. */
void b2em_blink_test(uint32_t cycles);

#endif /* B2E_MASTER_H */
