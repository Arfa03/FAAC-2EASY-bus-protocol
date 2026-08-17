/**
 * b2e_app.c — applicazione sniffer BUS-2EASY
 *
 * Da chiamare dal main() al posto del loop ECU:
 *
 *     b2e_init();
 *     b2e_app_run();      // non ritorna
 *
 * Console su USB CDC. Comandi (una lettera + invio):
 *
 *     s   stato: statistiche, ultimo frame, dispositivi censiti
 *     f   stampa continua dei frame che cambiano (toggle)
 *     a   stampa TUTTI i frame (toggle) — molto verboso
 *     e   esegue il censimento (1 secondo)
 *     c   azzera le statistiche
 *     ?   aiuto
 */

#include "b2e.h"
#include "b2e_master.h"
#include "stm32g4xx_hal.h"
#include "usbd_cdc_if.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static b2e_nodes_t nodes;
static b2e_frame_t last_frame;
static uint64_t    last_printed = ~(uint64_t)0;

static bool print_changes = true;
static bool print_all     = false;
static bool enrolling     = false;

/* ---------------------------------------------------------------------- */

static void say(const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    /* CDC_Transmit_FS ritorna BUSY se il buffer precedente non e' partito */
    for (int retry = 0; retry < 200; retry++) {
        if (CDC_Transmit_FS((uint8_t *)s, len) == USBD_OK) return;
        HAL_Delay(1);
    }
}

static void sayf(const char *fmt, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    say(buf);
}

/* ---------------------------------------------------------------------- */

static const char *slot_name(uint8_t s, char *tmp, uint32_t n)
{
    if (s < B2E_HEADER_BITS) {
        snprintf(tmp, n, "header");
    } else if (s >= B2E_SLOT_PHOTO_BASE && s < B2E_SLOT_CMD_BASE) {
        snprintf(tmp, n, "fotocellula DIP %u", (unsigned)(s - B2E_SLOT_PHOTO_BASE));
    } else if (s >= B2E_SLOT_CMD_BASE && s < B2E_SLOT_CMD_BASE + 16u) {
        snprintf(tmp, n, "comando DIP %u", (unsigned)(s - B2E_SLOT_CMD_BASE));
    } else {
        snprintf(tmp, n, "slot ignoto");
    }
    return tmp;
}

static void print_frame(const b2e_frame_t *f)
{
    static char bits[96];
    b2e_format_frame(f, bits, sizeof(bits));
    sayf("[%lu] %s\r\n", (unsigned long)f->seq, bits);

    /* elenca gli slot attivi oltre l'header */
    char tmp[40];
    for (uint8_t s = B2E_HEADER_BITS; s < B2E_FRAME_BITS; s++) {
        if ((f->mask >> s) & 1u) {
            sayf("      slot %2u  %s\r\n", (unsigned)s, slot_name(s, tmp, sizeof(tmp)));
        }
    }
}

static void print_status(void)
{
    const b2e_stats_t *st = b2e_get_stats();
    static char bits[96];

    sayf("\r\n--- BUS-2EASY ---\r\n");
    sayf("frame ok   : %lu\r\n", (unsigned long)st->frames_ok);
    sayf("frame bad  : %lu\r\n", (unsigned long)st->frames_bad);
    sayf("catture    : %lu\r\n", (unsigned long)st->captures);
    sayf("overrun    : %lu\r\n", (unsigned long)st->overrun);

    if (st->frames_ok + st->frames_bad) {
        uint32_t pct = (st->frames_ok * 1000u) / (st->frames_ok + st->frames_bad);
        sayf("validi     : %lu.%lu%%\r\n",
             (unsigned long)(pct / 10), (unsigned long)(pct % 10));
    }

    b2e_format_frame(&last_frame, bits, sizeof(bits));
    sayf("ultimo     : %s\r\n", bits);

    int32_t i100 = b2e_current_ma100();
    sayf("corrente   : %ld.%02ld mA\r\n",
         (long)(i100 / 100), (long)(i100 % 100));

    if (nodes.valid) {
        sayf("censiti    :");
        for (uint8_t s = B2E_HEADER_BITS; s < B2E_FRAME_BITS; s++) {
            if ((nodes.enrolled >> s) & 1u) sayf(" %u", (unsigned)s);
        }
        sayf("\r\n");
        sayf("appello    : %s\r\n", nodes.fault ? "ERRORE" : "ok");
        if (nodes.intruder) sayf("NON CENSITI attivi: 0x%08lX%08lX\r\n",
                                 (unsigned long)(nodes.intruder >> 32),
                                 (unsigned long)(nodes.intruder & 0xFFFFFFFFu));
    } else {
        sayf("censiti    : nessun censimento eseguito ('e')\r\n");
    }
    sayf("-----------------\r\n");
}

static void help(void)
{
    say("\r\n--- sniffer ---\r\n");
    say("s=stato  f=cambi  a=tutti  e=censimento  ?=aiuto\r\n");
    say("--- master (PILOTA IL BUS) ---\r\n");
    say("M=avvia  X=ferma  m=stato  E=censimento  B=blink test\r\n");
    say("+/- = sposta la finestra di campionamento (us)\r\n");
}

static void print_master(void)
{
    const b2em_state_t *m = b2em_get_state();
    sayf("\r\n--- MASTER ---\r\n");
    sayf("stato      : %s\r\n", m->running ? "ATTIVO" : "fermo");
    sayf("frame      : %lu\r\n", (unsigned long)m->frames);
    sayf("baseline   : %u  soglia: %u\r\n",
         (unsigned)m->baseline, (unsigned)m->threshold);
    sayf("risposte   :");
    for (uint8_t s = 8; s < 63; s++)
        if ((m->mask >> s) & 1u) sayf(" %u", (unsigned)s);
    sayf("\r\n");

    if (b2em_enrolled()) {
        uint64_t en = b2em_enrolled_mask();
        sayf("censiti    :");
        for (uint8_t s = 8; s < 63; s++)
            if ((en >> s) & 1u) sayf(" %u", (unsigned)s);
        sayf("\r\n");
        sayf("appello    : %s\r\n", b2em_fault() ? "ERRORE" : "ok");
        if (b2em_intruder()) sayf("INTRUSI    : slot non censiti attivi\r\n");
        /* consenso valutato sulle sole fotocellule censite (slot 16..31) */
        uint64_t photo = en & 0x00000000FFFF0000ull;
        sayf("consenso   : %s\r\n", b2em_motion_allowed(photo) ? "SI" : "NO");
    } else {
        sayf("censiti    : nessun censimento master ('E')\r\n");
    }
    /* profilo dei campioni: utile per trovare la finestra giusta */
    for (uint8_t base = 8; base < 48; base += 8) {
        sayf("raw %2u-%2u :", base, base + 7);
        for (uint8_t i = 0; i < 8; i++) sayf(" %4u", (unsigned)m->raw[base + i]);
        sayf("\r\n");
    }
    sayf("--------------\r\n");
}

/* ---------------------------------------------------------------------- */
/* Ricezione comandi da CDC.
 * usbd_cdc_if.c deve chiamare b2e_app_rx() da CDC_Receive_FS().            */

static volatile char cmd_pending = 0;

void b2e_app_rx(uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c > ' ') cmd_pending = c;
    }
}

static void handle_cmd(char c)
{
    switch (c) {
    case 's': print_status(); break;
    case 'f': print_changes = !print_changes;
              sayf("cambi: %s\r\n", print_changes ? "on" : "off"); break;
    case 'a': print_all = !print_all;
              sayf("tutti: %s\r\n", print_all ? "on" : "off"); break;
    case 'e': b2e_enroll_begin(); enrolling = true;
              say("censimento in corso, 1 secondo...\r\n"); break;
    case 'c': say("statistiche azzerate (riavvio richiesto)\r\n"); break;

    case 'M': say("MASTER ATTIVO - il bus e' pilotato da questa scheda\r\n");
              b2em_start(); break;
    case 'X': b2em_stop(); say("master fermo\r\n"); break;
    case 'K': b2em_calibrate(); say("calibrato\r\n"); print_master(); break;
    case 'm': print_master(); break;
    case 'E': b2em_enroll_begin();
              say("censimento master in corso, 1 secondo...\r\n"); break;
    case 'B': say("blink test 10 s su PA9: guarda il bus\r\n");
              b2em_blink_test(10); say("fine blink\r\n"); break;
    case '+': { static uint16_t w = 30; w += 5; b2em_set_sample_us(w);
                sayf("finestra: %u us\r\n", (unsigned)w); } break;
    case '-': { static uint16_t w = 30; w -= 5; b2em_set_sample_us(w);
                sayf("finestra: %u us\r\n", (unsigned)w); } break;
    case '>': { static uint16_t g = 40; g += 10; b2em_set_margin(g);
                sayf("margine: %u conteggi\r\n", (unsigned)g); } break;
    case '<': { static uint16_t g = 40; g -= 10; b2em_set_margin(g);
                sayf("margine: %u conteggi\r\n", (unsigned)g); } break;
    default:  help(); break;
    }
}

/* ---------------------------------------------------------------------- */

void b2e_app_run(void)
{
    b2e_frame_t f;
    uint32_t    t_status = 0;

    HAL_Delay(1500);          /* attende l'enumerazione USB */
    say("\r\nBUS-2EASY sniffer - STM32G474\r\n");
    help();

    for (;;) {
        while (b2e_poll(&f)) {
            last_frame = f;

            if (enrolling) {
                if (b2e_enroll_feed(&f, &nodes)) {
                    enrolling = false;
                    say("censimento completato\r\n");
                    print_status();
                }
            } else if (nodes.valid) {
                b2e_nodes_update(&nodes, &f);
            }

            if (print_all) {
                print_frame(&f);
            } else if (print_changes && f.mask != last_printed) {
                last_printed = f.mask;
                print_frame(&f);
            }
        }

        b2e_current_sample();
        b2em_poll();

        if (cmd_pending) {
            char c = cmd_pending;
            cmd_pending = 0;
            handle_cmd(c);
        }

        /* battito ogni 5 s, per capire subito se il bus e' fermo */
        if (HAL_GetTick() - t_status > 5000u) {
            t_status = HAL_GetTick();
            const b2e_stats_t *st = b2e_get_stats();
            static uint32_t prev_ok = 0;
            if (st->frames_ok == prev_ok) {
                say("... nessun frame negli ultimi 5 s\r\n");
            }
            prev_ok = st->frames_ok;
        }
    }
}
