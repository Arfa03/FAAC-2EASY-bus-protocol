# BUS-2EASY sniffer — STM32G474 (scheda Ignition Controller riadattata)

Progetto STM32CubeIDE ridotto al minimo: **nessun codice motore**. Accensione,
iniezione, pompa e IAC sono forzati a riposo all'avvio e non vengono piu' toccati.

Compilato e linkato senza errori: 75 KB flash, 13 KB RAM.

---

## Cablaggio

```
          BUS+ ──[100k]──┬──[22k]── GND        →  PC6   (connettore "DIN4")
                          │
                          └──────────────────── nodo centrale

          BUS− ──[10Ω]── GND                    →  PA3   (connettore "AIN2")
                     │
                     └── nodo caldo dello shunt
```

**Attenzione alle etichette:** sul connettore i DIN sono numerati al contrario dei
pin. Il segnale del bus va su **PC6**, marcato **DIN4**. È l'unico dei quattro che
porti `TIM3_CH1`.

Alimenta la scheda **da USB**, non dalla batteria: elimina anelli di massa e
l'OR-ing LM66100 lo consente già.

### Modifica hardware necessaria per il canale corrente

Il condensatore di filtro su AIN2 forma con i 10 kΩ serie una costante di tempo di
~1 ms: cancellerebbe un impulso da 100 µs. **Portalo a 1 nF o rimuovilo.**

Senza questa modifica il canale tensione funziona lo stesso; solo la lettura di
corrente resta piatta.

---

## Risorse usate

| Risorsa | Uso |
|---|---|
| PC6 | TIM3_CH1 input capture, fronte di discesa |
| TIM3 | base tempi 1 µs (PSC 169 da 170 MHz) |
| DMA1_Ch3 | cattura circolare, 512 campioni, nessun interrupt |
| PA3 | OPAMP1 VINP1, PGA ×4 |
| ADC1 | canale interno VOPAMP1, conversione continua |
| USB CDC | console |

Clock invariato rispetto al progetto originale: HSE 8 MHz → PLL /2 ×85 /2 = 170 MHz,
HSI48 + CRS per l'USB, CSS attivo. Se il quarzo cade il firmware si ferma: con HSI la
temporizzazione del bus non sarebbe più affidabile.

---

## Console USB

```
s   stato: statistiche, ultimo frame, dispositivi censiti, corrente
f   stampa i frame quando cambiano (default on)
a   stampa tutti i frame (50/s, verboso)
e   censimento: accumula 1 secondo e salva la maschera
c   azzera
?   aiuto
```

Uscita tipica con una fotocellula DIP 0000 e un selettore DIP 0000:

```
[1423] 11111111 00000000 10000000 00000000 01000000 00000000 00000000 0000000
      slot 16  fotocellula DIP 0
      slot 33  comando DIP 1
```

---

## Collaudo

**1 — Frame validi.** Con XBR2 come master, comando `s`: `validi` deve essere sopra
il 99.9% e `overrun` a zero.

Se i frame validi sono pochi, in ordine di probabilità: partitore assente o sbagliato,
massa non in comune, quarzo non montato.

**2 — Fascio interrotto.** Lo slot 16 deve sparire entro un frame (20 ms).

**3 — Censimento.** Comando `e` con fotocellula allineata e libera. Poi stacca l'RX:
`appello: ERRORE` entro ~100 ms.

**4 — Corrente.** A riposo deve leggere ~15 mA. Se legge zero o fondo scala, il
sospetto è il condensatore su AIN2 non modificato.

---

## Cosa questo firmware NON fa

Non trasmette e non può farlo: PC6 è configurato come input capture, e nessun altro
pin viene toccato dopo l'init. Le uscite di potenza sono spente in `MX_GPIO_Init()`
e nessun codice le riaccende.

Lo stadio master (generazione dei 64 slot, pull-up su Vin, MOSFET dell'uscita
iniettore cablato a BUS+) è un modulo separato, da scrivere solo dopo che lo sniffer
legge senza errori per un'ora.

---

## Struttura

```
Core/Inc/    b2e.h  main.h  stm32g4xx_hal_conf.h  stm32g4xx_it.h
Core/Src/    b2e.c  b2e_app.c  main.c  stm32g4xx_it.c
             stm32g4xx_hal_msp.c  syscalls.c  sysmem.c  system_stm32g4xx.c
USB_Device/  stack CDC (usbd_cdc_if.c chiama b2e_app_rx)
Drivers/     HAL G4 + CMSIS, con stm32g4xx_hal_opamp.c aggiunto
```

`HAL_OPAMP_MODULE_ENABLED` è già attivo in `stm32g4xx_hal_conf.h` e il driver OPAMP
è già nel progetto: non serve rigenerare da CubeMX. Il `.ioc` è stato rimosso di
proposito — se lo rigeneri, CubeMX riscrive `main.c` e reintroduce le periferiche ECU.
