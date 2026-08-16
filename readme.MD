# FAAC BUS-2EASY — reverse engineering and an STM32 master

*[🇮🇹 Versione italiana](#-versione-italiana)*

Reconstruction of FAAC's proprietary **BUS 2easy** protocol: the two-wire bus used
by FAAC gate controllers to connect photocells, safety edges and command devices.

FAAC publishes no specification for this protocol. Everything below was derived
from logic-analyser measurements and **verified by reimplementation**: a master
rebuilt on an STM32G474 drives the bus, powers the nodes, and correctly reads a
genuine FAAC photocell pair and key selector.

> ⚠️ **Not for real installations.** A gate system whose safety devices are handled
> by home-built electronics falls outside EN 12453 / EN 12445 conformity. This is
> technical documentation and private experimentation.

---

## At a glance

| | |
|---|---|
| **Topology** | two wires, non-polarised, power and data on the same pair |
| **Voltage** | 13.65 V regulated, ~12 Ω source impedance |
| **Encoding** | pulse-distance, 99 µs low pulses |
| **Slot** | 300 µs — one pulse = bit 0, two pulses = bit 1 |
| **Frame** | 64 slots + gap, **20.0 ms**, 50 frames/s |
| **Header** | first 8 slots always 1, used for synchronisation |
| **Addressing** | one slot per device, address = value of the 4 DIP switches |
| **Checksum** | none. Redundancy is the 50 Hz repetition |

```
frame:  11111111 00000000 10000000 00000000 01000000 00000000 00000000 0000000
        └header┘                   ▲                  ▲
                                   slot 16            slot 33
                                   photocell          key selector
```

### Slot map

| Slots | Block |
|---|---|
| 0–7 | synchronisation header |
| 8–15 | unknown (probably safety edges) |
| **16–31** | **photocells** — `slot = 16 + DIP` |
| **32–47** | **command devices** — `slot = 32 + DIP` |
| 48–63 | unknown (probably encoders) |

---

## Repository contents

| Path | Description |
|---|---|
| [`BUS-2EASY-protocol.md`](BUS-2EASY-protocol.md) | **Full specification.** Physical layer, encoding, framing, slot map, semantics, read-out circuits, how to implement a master |
| `firmware/` | STM32CubeIDE project for STM32G474: sniffer + master, USB CDC console |
| `captures/` | sigrok/PulseView captures used for the reconstruction |

---

## Minimal circuit to read the bus

Two resistors. Loading on the line is a few hundred µA.

```
BUS signal ──[ 100k ]──┬──── logic input
                        │
                    [ 15k ]
                        │
                       GND ──── common with bus negative
```

> **Impedance matters more than the ratio.** If the input has a filter capacitor —
> typical of industrial digital inputs — the time constant destroys the signal.
> With 10 nF and an 18 kΩ equivalent divider, τ = 180 µs against 99 µs pulses: the
> pin sits at the average value and no edges appear at all. Use a low-impedance
> divider, or remove the capacitor.

A minimal master additionally needs a pull-up and a low-side MOSFET: see §9.2 of
the specification.

---

## Functional safety

The photocell channel is **inherently fail-safe**. Verified across every failure
mode:

| Condition | Bit |
|---|---|
| Beam clear, pair aligned | **1** |
| Beam interrupted | 0 |
| Misalignment without obstacle | 0 |
| RX or TX disconnected from the bus | 0 |

No observed combination leaves the bit at 1 while a fault is present. A master may
treat **absence of the bit as an obstacle** with no exceptions. Transition latency:
one frame, 20 ms.

Two warnings for implementers:

**Slots 38 and 39 (Stop NC) use inverted logic**: `bit = 0` means STOP asserted.
Treating them like the others defeats the very mechanism that makes them safe.

**SETUP does not authenticate.** Enrolment is a census performed by the master:
slaves respond regardless, before and after. A device with the same DIP settings
produces bits indistinguishable from the legitimate one. The protocol has no
authentication of any kind — a node's only identity is the slot number it occupies.

---

## Status

Verified against FAAC XBR2 (BUS-RELAY), XP20B D (photocells), XK10 B (key
selector), and E721 (control board, in standby mode).

**Settled:** physical layer, encoding, framing, addressing of photocells and
command devices, bit semantics, fail-safe behaviour, enrolment.

**Open:**

- contents of slots 8–15 and 48–63 — a safety edge and an encoder are needed
- how a device detects the master type (control board vs XBR2/XBR4), which changes
  how it interprets its DIP switches
- exit from low-consumption mode, the only known case where the master transmits
- the transition between the 20 ms operating frame and the standby mode with the
  line powered down

Contributions welcome, particularly measurements with devices that were not
available here.

---

## License

Documentation and code released for educational and research use.
FAAC, BUS 2easy, XP20B, XK10B, XBR2 and E721 are trademarks of FAAC S.p.A.
This project is not affiliated with or endorsed by FAAC.

---
---

## 🇮🇹 Versione italiana

*[⬆ Back to English](#faac-bus-2easy--reverse-engineering-and-an-stm32-master)*

Ricostruzione del protocollo proprietario **BUS 2easy** di FAAC, il bus a due fili
usato dalle centrali per cancelli automatici per collegare fotocellule, coste di
sicurezza e dispositivi di comando.

FAAC non pubblica specifiche di questo protocollo. Quanto segue è ricavato da
misure con analizzatore logico e **verificato per implementazione**: un master
ricostruito su STM32G474 genera il bus, alimenta i nodi e legge correttamente una
coppia di fotocellule e un selettore a chiave FAAC originali.

> ⚠️ **Non usare per impianti reali.** Un impianto in cui i dispositivi di sicurezza
> sono gestiti da elettronica autocostruita esce dalla conformità EN 12453 / EN 12445.
> Questo lavoro è documentazione tecnica e sperimentazione privata.

---

### In breve

| | |
|---|---|
| **Topologia** | due fili non polarizzati, alimentazione e dati sulla stessa coppia |
| **Tensione** | 13.65 V regolati, ~12 Ω di impedenza di sorgente |
| **Codifica** | pulse-distance, impulsi bassi da 99 µs |
| **Slot** | 300 µs — un impulso = bit 0, due impulsi = bit 1 |
| **Frame** | 64 slot + gap, **20.0 ms**, 50 frame/s |
| **Header** | 8 slot iniziali sempre a 1, servono da sincronismo |
| **Indirizzamento** | 1 slot per dispositivo, indirizzo = valore dei 4 DIP switch |
| **Checksum** | nessuno. La ridondanza è la ripetizione a 50 Hz |

```
frame:  11111111 00000000 10000000 00000000 01000000 00000000 00000000 0000000
        └header┘                   ▲                  ▲
                                   slot 16            slot 33
                                   fotocellula        selettore a chiave
```

#### Mappa degli slot

| Slot | Blocco |
|---|---|
| 0–7 | header di sincronismo |
| 8–15 | ignoto (probabilmente coste di sicurezza) |
| **16–31** | **fotocellule** — `slot = 16 + DIP` |
| **32–47** | **dispositivi di comando** — `slot = 32 + DIP` |
| 48–63 | ignoto (probabilmente encoder) |

---

### Contenuto del repository

| File | Descrizione |
|---|---|
| [`BUS-2EASY-protocol.md`](BUS-2EASY-protocol.md) | **Specifica completa.** Livello fisico, codifica, framing, mappa degli slot, semantica, circuiti di lettura, implementazione di un master |
| `firmware/` | Progetto STM32CubeIDE per STM32G474: sniffer + master, console su USB CDC |
| `captures/` | Catture sigrok/PulseView usate per la ricostruzione |

---

### Circuito minimo per leggere il bus

Due resistori. Il carico sulla linea è di poche centinaia di µA.

```
BUS segnale ──[ 100k ]──┬──── ingresso logico
                         │
                     [ 15k ]
                         │
                        GND ──── comune col negativo del bus
```

> **L'impedenza conta più del rapporto.** Se l'ingresso ha un condensatore di
> filtro — tipico degli ingressi digitali industriali — la costante di tempo
> annulla il segnale. Con 10 nF e un partitore da 18 kΩ equivalenti, τ = 180 µs
> contro impulsi da 99 µs: il pin resta fermo al valor medio e non si vede nessun
> fronte. Usa un partitore a bassa impedenza o rimuovi il condensatore.

Per un master minimo servono in più un pull-up e un MOSFET low-side: vedi §9.2
della specifica.

---

### Sicurezza funzionale

Il canale fotocellule è **intrinsecamente fail-safe**. Verificato su tutti i modi
di guasto:

| Condizione | Bit |
|---|---|
| Fascio libero, coppia allineata | **1** |
| Fascio interrotto | 0 |
| Disallineamento senza ostacolo | 0 |
| RX o TX scollegato dal bus | 0 |

Nessuna combinazione osservata lascia il bit a 1 in presenza di guasto. Un master
può trattare **assenza di bit = ostacolo** senza eccezioni. Latenza di transizione:
un frame, 20 ms.

Due avvertenze per chi implementa:

**Gli slot 38 e 39 (Stop NC) sono a logica invertita**: `bit = 0` significa STOP
attivo. Trattarli come gli altri annulla proprio il meccanismo che li rende sicuri.

**Il SETUP non autentica.** L'iscrizione è un censimento lato master: gli slave
rispondono comunque, prima e dopo. Un dispositivo con gli stessi DIP produce bit
indistinguibili da quelli del legittimo. Il protocollo non ha nessuna forma di
autenticazione — l'unica identità di un nodo è il numero di slot che occupa.

---

### Stato

Verificato con FAAC XBR2 (BUS-RELAY), XP20B D (fotocellule), XK10 B (selettore),
E721 (centrale, in modalità standby).

**Chiuso:** livello fisico, codifica, framing, indirizzamento di fotocellule e
dispositivi di comando, semantica dei bit, comportamento fail-safe, censimento.

**Aperto:**

- contenuto degli slot 8–15 e 48–63 — servono una costa di sicurezza e un encoder
- come un dispositivo riconosce il tipo di master (centrale vs XBR2/XBR4), che ne
  cambia l'interpretazione dei DIP
- uscita dalla modalità basso consumo, unico caso noto in cui il master trasmette
- transizione fra il frame operativo a 20 ms e la modalità standby a linea spenta

Contributi benvenuti, in particolare misure con dispositivi che qui non erano
disponibili.

---

### Licenza

Documentazione e codice rilasciati per uso didattico e di ricerca.
FAAC, BUS 2easy, XP20B, XK10B, XBR2 e E721 sono marchi di FAAC S.p.A.
Questo progetto non è affiliato né approvato da FAAC.