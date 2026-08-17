# FAAC BUS-2EASY — protocol reverse engineering and an STM32 master

*[🇮🇹 Versione italiana](#-versione-italiana)*

Reconstruction of FAAC's proprietary **BUS 2easy** protocol: the two-wire bus used
by FAAC gate controllers to connect photocells, safety edges and command devices.

FAAC publishes no specification for this protocol. Everything here was derived from
logic-analyser measurements and **verified by reimplementation**: a master rebuilt on
an STM32G474 drives the bus, powers the nodes, enrols them and reads a genuine FAAC
photocell pair and key selector.

> ⚠️ **Not for real installations.** A gate system whose safety devices are handled by
> home-built electronics falls outside EN 12453 / EN 12445 conformity. This is
> technical documentation and private experimentation.

---

## Contents

- [Protocol](#protocol)
  - [Physical layer](#physical-layer)
  - [Bit encoding](#bit-encoding)
  - [Frame structure](#frame-structure)
  - [Slot map](#slot-map)
  - [Bit semantics](#bit-semantics)
  - [Enrolment](#enrolment)
  - [Standby mode](#standby-mode)
- [Hardware](#hardware)
  - [Reading the bus](#reading-the-bus-passive-sniffer)
  - [Master output stage](#master-output-stage)
- [Firmware](#firmware)
- [Repository layout](#repository-layout)
- [Status and open questions](#status-and-open-questions)

---

# Protocol

## Physical layer

Two wires, **non-polarised** on the accessory side: accessories have an input diode
bridge and cannot tell the two wires apart. Encoders and master/slave board pairing
are the exception and do require polarity.

On the master side the line is **single-ended**: one wire is ground, the other carries
both power and data.

| Parameter | Value |
|---|---|
| Line voltage | 24 V nominal (13.6 V measured average on a FAAC XBR2, see note) |
| Source impedance at rest | ~12 Ω |
| Protection | current limit between 231 and 427 mA, with **automatic retry** after 1–2 s |
| Logic low level | 0 V |
| State between frames | high |
| Edges | linear ramps, ~5 µs, both rising and falling |
| Max line current | 500 mA |
| Max cable length | 100 m, 0.5 mm² max |
| Typical node draw | 10 mA per unit |

> A multimeter on the bus reads an *average*, not a level: with the duty cycle of a
> live frame the average sits well below the peak. Always measure the high level with
> a scope.

**The master output stage is fully active.** Both edges are driven with a controlled
slew rate — there is no resistive pull-up or pull-down. The measured ~12 Ω source
impedance is the output resistance of the regulator feeding the line, not a component.
Consequently a genuine master dissipates only a few milliwatts while powering the
whole bus.

### The master must stay compliant

This is the single least obvious property of the bus, and getting it wrong makes
devices reject the master.

The high side must be **soft enough for a slave to pull the line down**. A saturated
transistor presenting a fraction of an ohm holds the line so rigidly that slaves
cannot signal, and they report an address conflict. A genuine XBR2 keeps ~12 Ω of
source impedance for exactly this reason.

A rebuilt master needs a series resistor — see [Master output stage](#master-output-stage).

### Slave to master direction

**Slaves pull the line down in the voltage domain.** The master does not regenerate
anything: it reads the level.

Slaves also draw a small current spike while responding (~2 mA over ~100 µs), but that
is a consequence of pulling the line toward ground through the master's impedance, not
a separate signalling channel.

---

## Bit encoding

**Pulse-distance**, self-clocking. Every low pulse lasts ~99 µs. The information is in
the **distance between consecutive falling edges**.

```
bit 0   — one pulse in the slot ────────────────────────────────────

     ┐        ┌──────────────────────────────┐
     │  99µs  │            201 µs            │
     └────────┘                              └───  →  300 µs


bit 1   — two pulses in the slot ───────────────────────────────────

     ┐        ┌────────┐        ┌────────┐
     │  99µs  │  54µs  │  99µs  │  44µs  │
     └────────┘        └────────┘        └───  →  150 + 150 = 300 µs
```

| Symbol | Falling-edge distance | Meaning |
|---|---|---|
| `0` | 300 µs | empty slot |
| `1` | 150 µs + 150 µs | occupied slot |

**Decoding.** Capture falling edges, compute the intervals. Interval > ~220 µs → `0`
(consumes 1 slot). Interval ≤ ~220 µs → `1` (consumes 2 intervals, i.e. 1 slot).

### Measured timing constants

| Quantity | Value | Observed spread |
|---|---|---|
| Low pulse width | 99 µs | 98–101 µs |
| Slot | **299.876 µs** | ±0.3 µs |
| Half-slot | ~150 µs | |
| Slot rate | 3334.7 Hz | |

The slot is nominally 300 µs; the 0.04% deviation is within the analyser's own clock
error and is not worth chasing.

---

## Frame structure

```
│◄──────────────── 20.0 ms ────────────────►│
│                                            │
├──── header ────┬──── data ────┬─── gap ───┤
│ 8 slots, all 1 │   55 slots   │  1.05 ms  │
│  slots 0…7     │  slots 8…62  │  (high)   │
```

| Field | Value |
|---|---|
| Frame period | **20.0 ms** (50 frames/s), continuous |
| Header | slots 0–7, always `11111111` |
| Decodable slots | 63 (slot 63 is absorbed into the gap → 64 total) |
| Inter-frame gap | ~1.05 ms, line high |

**Synchronisation.** The gap is the only interval longer than 500 µs, so it is the
frame boundary. Alternatively, look for the run of 8 consecutive `1` bits.

**The master transmits no data.** The header is invariant and no data bit has ever
been observed to originate from the master, not even during enrolment. All information
comes from the slaves.

The only documented case where a master should transmit is waking devices from
low-consumption mode. Not observed here.

**No checksum.** 63 bits with no parity or CRC, verified with two devices active
simultaneously. Redundancy is purely temporal: 50 repetitions per second.

### Timing signatures

Inside a `1` slot the exact position of the second pulse depends on which device
generated it. The signatures are stable over hundreds of frames and mutually
distinguishable.

| Source | gap 1 | gap 2 |
|---|---|---|
| master (header) | 153.1 µs | 148.2 µs |
| XP20B (photocell) | 150.1 µs | 151.2 µs |
| XK10B (key selector) | 157.4 µs | 143.9 µs |

*gap 1* is the high interval between the first and second pulse of the slot, *gap 2*
the one between the second pulse and the start of the next slot.

A master can use these to check that a bit came from the expected device family. It is
diagnostics, not a security mechanism.

---

## Slot map

The 64 slots are divided into blocks by device class. Each device occupies **one slot**
and its address is the value of its 4 DIP switches.

| Slots | Block | Formula |
|---|---|---|
| 0–7 | synchronisation header | — |
| 8–15 | **unknown** — never observed active | — |
| **16–31** | **photocells** | `slot = 16 + DIP` |
| **32–47** | **command devices** | `slot = 32 + DIP` |
| 48–63 | **unknown** — never observed active | — |

Safety edges and encoders presumably live in blocks 8–15 and 48–63, but these were not
measured.

Photocell DIP switches and command-device DIP switches **do not interfere**: separate
address spaces, separate slot blocks, no collisions.

### Photocell block (16–31)

`slot = 16 + DIP value`, DIP1 as MSB.

| DIP | Slot | Function on a control board |
|---|---|---|
| 0000 | 16 | opening safety |
| 0001 | 17 | opening safety |
| 0010 | 18 | opening safety |
| 0011 | 19 | opening safety |
| 0100 | 20 | opening + closing safety |
| 0101 | 21 | opening + closing safety |
| 0110 | 22 | opening safety |
| 0111 | 23 | opening safety |
| 1000 | 24 | closing safety |
| 1001–1110 | 25–30 | closing safety |
| 1111 | 31 | OPEN pulse |

**The function class does not move the slot.** The slot depends only on the numeric
value of the DIP switches; it is the master that, knowing the table, interprets the
address as an opening safety, a closing safety, both, or an OPEN pulse.

Manual limits: 6 opening pairs, 7 closing pairs, 2 opening+closing, 1 OPEN pulse.

### Command device block (32–47)

`slot = 32 + DIP value`.

Connected to a **control board**, the DIP code *is* the command:

| Slot | Command | Slot | Command |
|---|---|---|---|
| 32 | Open A_1 | 40 | Close |
| 33 | Open A_2 | 41 | Open B_1 |
| 34 | Open A_3 | 42 | Open B_2 |
| 35 | Open A_4 | 43 | Open B_3 |
| 36 | Open A_5 | 44 | Open B_4 |
| 37 | Stop | 45 | Open B_5 |
| 38 | **Stop NC_1** | 46 | not used |
| 39 | **Stop NC_2** | 47 | not used |

> ⚠️ **Slots 38 and 39 use inverted logic.** Stop NC asserts a stop even when the
> device is disconnected from the bus. A conforming master must treat **bit = 0 as
> STOP asserted** on these two slots.

Connected to an **XBR2/XBR4 relay interface**, the same space reads as
`slot = 32 + (ID−1)×4 + (Relay−1)`, with ID 1–4 and relay 1–4. The device detects the
master type at power-up and switches DIP interpretation on its own. **How it detects
the master type is unknown.**

---

## Bit semantics

### Photocells — fail-safe behaviour

A TX+RX pair occupies **one slot**. The bit is generated by the receiver and requires
the optical link to be established.

| Condition | Bit |
|---|---|
| Beam clear, pair aligned and working | **1** |
| Beam interrupted (obstacle) | **0** |
| Misalignment without obstacle | **0** |
| RX disconnected from the bus | **0** |
| TX disconnected from the bus | **0** |
| RX only (TX absent) | **0** |
| TX only (RX absent) | **0** |

**Every failure mode converges on the safe state.** No observed combination leaves the
bit at 1 while a fault is present, so a master may treat **absence of the bit as an
obstacle** with no exceptions.

- **Transition latency: one frame, 20 ms.**
- The bus **does not distinguish** an obstacle from misalignment.
- DIP code 1111 (OPEN pulse) behaves like an ordinary status bit on slot 31.

### Command devices

The command bit is a **level, not a pulse**: it stays at 1 for the whole duration of
the actuation. Edge detection is the master's job.

**Validity flag.** Alongside the command bit, the device permanently asserts a second
bit on the **companion slot** (address XOR 1), **if and only if its configuration is
usable on the connected master**.

Observed on an XBR2, which has 2 relays:

| DIP | ID / Relay | command slot | flag slot | Relay switches? |
|---|---|---|---|---|
| 0000 | ID1 R1 | 32 | 33 | yes |
| 0001 | ID1 R2 | 33 | 32 | yes |
| 0010 | ID1 R3 | 34 | — | **no**, LED only |
| 0011 | ID1 R4 | 35 | — | **no**, LED only |
| 0100 | ID2 R1 | 36 | 37 | yes |
| 0110 | ID2 R3 | 38 | — | **no**, LED only |
| 1000 | ID3 R1 | 40 | 41 | yes |
| 1110 / 1111 | not used | — | — | device stays silent |

The flag carries the slave's timing signature: it is the **device** that knows its
configuration is unusable, not the master.

---

## Enrolment

**A device responds before it has been enrolled**, in every frame. During the SETUP
procedure the bus traffic does not change by a single bit.

Therefore **enrolment is a census performed entirely by the master**. It observes which
slots are active and records them as expected; from then on an expected slot that stops
responding is a roll-call error. The slave has no enrolment state and does not need to
be told anything.

For a rebuilt master, **enrolling means saving the mask of active slots**.

> ⚠️ **SETUP does not authenticate.** A device with the same DIP settings produces bits
> indistinguishable from the legitimate one. The protocol has no authentication of any
> kind — a node's only identity is the slot number it occupies.

---

## Standby mode

A control board can **power the line down** and probe it periodically.

```
│◄────────────────────── 3.1545 s ──────────────────────►│

├─ preamble ──┬─ 64 slots ┬─ tail ─┬──────── idle ───────┤
│  120.27 ms  │  19.2 ms  │ 1.77ms │       3.013 s       │
│    high     │           │  high  │         LOW         │
```

- all 64 slots carry a single pulse (bit 0), **47 µs** wide instead of 99
- no header
- 4.5% duty cycle: the line sits at 0 V for **95.5%** of the time

No accessory can power itself in this state, so nothing ever responds. This is not an
enumeration phase, it is a probe on a de-energised line.

A control board fed with DC and no AC mains believes it is running on battery and enters
this state. Capturing operational traffic requires proper AC supply.

---

# Hardware

## Reading the bus (passive sniffer)

Two resistors. Loading on the line is a few hundred µA.

```
BUS signal ──[ R1 ]──┬──── logic input
                      │
                   [ R2 ]
                      │
                     GND ────── common with bus negative
```

| Line voltage | R1 | R2 | Output |
|---|---|---|---|
| 24 V | 100 kΩ | 15 kΩ | 3.1 V |
| 13.6 V | 100 kΩ | 22 kΩ | 2.5 V |

> **Impedance matters more than the ratio.** If the input has a filter capacitor —
> typical of industrial digital inputs — the time constant destroys the signal. With
> 10 nF and an 18 kΩ equivalent divider, τ = 180 µs against 99 µs pulses: the pin sits
> at the average value and no edges appear at all. Use a low-impedance divider, or
> remove the capacitor. A multimeter on the input should read the *average* of a
> swinging signal, not a steady level.

Sampling: **1 MSa/s** is enough; use 2 MSa/s to resolve the timing signatures.
Ground must be common between the bus negative and the instrument.

---

## Master output stage

The line is driven by a **PNP high side**. It is never shorted: to produce a low level
the transistor simply turns off and the line falls on its own. Total dissipation is in
the milliwatt range.

```
                    +24 V
                      │
          ┌───────────┴───────────┐
          │                       │
       [ R1 ]                 E ──┴──
        22 kΩ                   Q1 BC327   (PNP)
          │                     │
          ├─────────────── B ───┤
          │                     │
       [ R2 ]                 C ──[ R3 ]──┬──────────── BUS +  ──→ nodes
        4.7 kΩ                   100 Ω    │
          │                               │
   drain of Q2                        [ 100 kΩ ]
   (N-channel MOSFET,                     │
    logic level)                          ├──────────── MCU sense input
          │                            [ 15 kΩ ]
          │                               │
         GND ──────────────────────────────┴────────── BUS −

   MCU output ──[ 100 Ω ]── gate of Q2
   gate ──[ 10 kΩ ]── GND
```

| Ref | Value | Role |
|---|---|---|
| Q1 | **BC327** (PNP) | 45 V / 800 mA. Avoid 25 V parts such as the S8550 |
| Q2 | any logic-level N-MOSFET | inverting stage; ≥ 30 V, a few mA is plenty |
| **R2** | **4.7 kΩ** | **limits base current. Not optional — see below** |
| R1 | 22 kΩ | turns Q1 off when Q2 is not conducting |
| **R3** | **100 Ω** | **keeps the source compliant. Not optional — see below** |
| divider | 100 kΩ / 15 kΩ | line sensing, **downstream of R3** |

### R2 — base current limiting

With the base wired straight to the drain, the base-emitter junction has nothing in
series when Q2 conducts: V_BE clamps at ~0.8 V but the current is unlimited and the
junction is destroyed on the first pulse.

With R2 = 4.7 kΩ the base sits at ~23.2 V when conducting, so I_B ≈ 4.9 mA. Saturation
is designed with a forced β of 10–20, not the datasheet figure, which gives 49–98 mA of
guaranteed collector current against the 10–25 mA the nodes draw.

### R3 — source compliance

Without it the saturated PNP holds the line too rigidly for slaves to pull it down, and
devices report an address conflict. R3 reproduces the source impedance of a genuine
master. It conducts only while Q1 is on, so it dissipates only what the load draws:
25 mA through 100 Ω is 63 mW.

### Sense point

The divider must sit **downstream of R3**. Upstream it reads the collector, where the
saturated PNP pins the voltage and a slave's pulse is invisible.

### Line discharge

When Q1 turns off, the node current discharges the line. With few nodes and short cable
this is sufficient. If the falling edge is too slow, add a permanent resistor from BUS+
to ground — 2.2 kΩ dissipates 260 mW and is never shorted, since there is no low-side
device.

### Logic

| MCU output | Q2 | Q1 base | Q1 | Line |
|---|---|---|---|---|
| **high** | conducting | pulled down via R2 | **on** | **high** |
| **low** | off | held at +24 by R1 | off | **released → falls** |

---

# Firmware

STM32CubeIDE project for **STM32G474RCT6** at 170 MHz from an 8 MHz crystal. It contains
both a passive sniffer and a full master, with a USB CDC console. The master is
configured at start-up but **stays stopped**: it drives the bus only after an explicit
`M` command.

> **This firmware targets a custom STM32G474 board.** Pin assignments, clock
> configuration and the GPIO safe-state code in `main.c` are specific to that board and
> **must be adapted** to whatever hardware you use. What is portable is the protocol
> logic in `b2e.c` and `b2e_master.c`: the timing, the decoder, the census and the
> consent rules. Anything that touches a pin, a timer instance or a DMA channel needs to
> be reviewed against your own schematic.
>
> If you move the master output to a different pin, keep it on a timer that can generate
> the update and two compare interrupts. If you move the sense input, it must be readable
> as a plain GPIO level and, for the sniffer, sit on a timer input-capture channel.

### Peripherals

| Resource | Use |
|---|---|
| PC6 | TIM3_CH1 input capture, falling edge — sniffer, and level sense for the master |
| TIM3 | 1 µs time base (PSC 169) |
| DMA1_Ch3 | circular capture, 512 samples, no interrupts |
| PA9 | master output, drives Q2 |
| TIM1 | 150 µs half-slot, CC1 = pulse end, CC2 = sampling instant |
| USB CDC | console |

These are the pins of the reference board; change them to match yours.

The master reads the line level through `PC6->IDR`, so both roles share one pin: the
sniffer decodes the whole frame while the master samples its own reception window.

### Key constants

`Core/Inc/b2e_master.h`

```c
#define B2EM_HALF_US          150u   /* half slot                     */
#define B2EM_PULSE_US         112u   /* commanded pulse width         */
#define B2EM_SLOTS             64u   /* slots per frame               */
#define B2EM_GAP_US          1050u   /* inter-frame gap, line high    */
#define B2EM_SAMPLE_US_DEFAULT 30u   /* sampling instant in the slot  */
```

> **Why 112 and not 99.** The commanded value is the time the transistor is held off.
> The line takes a few microseconds to cross the receiver threshold, and that time is
> subtracted from the pulse. 112 µs commanded yields **99.5 µs measured** on the bus with
> the stage above. Anyone rebuilding this must re-measure: the offset depends on the
> discharge path.

The sampling instant is where the master looks for a slave's response inside the odd
half-slot. The usable window measured with this output stage is **20–60 µs**; 30 µs sits
in the middle.

### Enrolment and roll-call

```c
#define B2EM_ENROLL_FRAMES   50u   /* 1 second at 50 frames/s          */
#define B2EM_MISSING_LIMIT    5u   /* ~100 ms before declaring a fault */
```

`b2em_motion_allowed(photo_mask)` returns the movement consent and denies it on every
doubtful condition: master stopped, no census, roll-call error, uncensused slots active,
a required photocell not clear, or a Stop NC slot at 0.

Two deliberate exclusions:

- **Slots 38 and 39 are excluded from the roll-call.** With inverted logic a present,
  idle device holds the bit at 0 exactly like a vanished one. Their absence already
  produces the safe state, so no roll-call is needed.
- **Intruder detection covers only slots 8–31.** Command bits are absent at rest by
  nature, so they can never be enrolled and would raise a permanent false positive.

### Console

Connect to the CDC port at any baud rate.

```
--- sniffer ---
s   status: statistics, last frame, enrolled devices
f   print frames when they change (default on)
a   print every frame (50/s, very verbose)
e   sniffer census: accumulate 1 second and store the mask

--- master (DRIVES THE BUS) ---
M   start        X   stop
m   master status: responses, census, roll-call, consent, raw slot levels
E   master census
B   blink test: toggles the line at 1 Hz, for static checks
+/- move the sampling instant by 5 µs
```

The master is configured at start-up but **stays stopped**: it only drives the bus after
an explicit `M`.

Typical output with a photocell at DIP 0000 and a key selector at DIP 0001:

```
[1423] 11111111 00000000 10000000 00000000 01000000 00000000 00000000 0000000
      slot 16  photocell DIP 0
      slot 33  command DIP 1
```

### Commissioning

1. **Current-limit the supply to 300 mA** during bring-up.
2. Check Q1 with a diode test before fitting it, and verify with a meter which pin goes
   to +24 V and which to BUS+.
3. `B` (blink test) and a meter on BUS+: it must alternate between the full line voltage
   and 0 every half second.
4. Start the master with `M`, then `m`: adjust the sampling window with `+`/`-` until a
   connected device shows up in `raw`.
5. `E` to enrol, with photocells aligned and clear.
6. Interrupt the beam: the photocell slot must clear within one frame and consent must
   drop to `NO`.

---

# Repository layout

```
readme.MD           this file
firmware/           STM32CubeIDE project (STM32G474RCT6)
  Core/Src/b2e.c            sniffer: capture, decode, census
  Core/Src/b2e_master.c     master: slot generation and response sensing
  Core/Src/b2e_app.c        USB CDC console
captures/           sigrok/PulseView captures used for the reconstruction
  XBR2 -- XK10B/      key selector, DIP sweep, SETUP, empty bus
  XBR2 -- XP20/       photocell pair, DIP sweep, fail-safe tests
```

Captures are 2 MSa/s, single channel, taken through a passive divider.

> sigrok splits long captures into `logic-1-1 … logic-1-N`. **All** chunks must be
> concatenated before analysis; reading only the first one makes a capture look empty.

---

# Status and open questions

Verified against FAAC XBR2 (BUS-RELAY), XP20B D (photocells), XK10 B (key selector), and
an E721 control board in standby.

**Settled:** physical layer, bit encoding, framing, addressing of photocells and command
devices, bit semantics, fail-safe behaviour, enrolment, and a working master.

**Open:**

| Topic | Impact |
|---|---|
| How a device detects the master type (control board vs XBR2/XBR4) | High: it changes DIP interpretation for command devices. Photocells are unaffected |
| Contents of slots 8–15 and 48–63 | Medium: presumably safety edges and encoders |
| Exit from low-consumption mode | The only known case where a master transmits |
| Transition between the 20 ms operating frame and standby | Medium |
| Exact current threshold of the line protection | Low: measured between 231 and 427 mA |
| Behaviour on address conflict | Low |

Contributions welcome, particularly measurements with devices not available here.

---

# License

Documentation and code released for educational and research use.
FAAC, BUS 2easy, XP20B, XK10B, XBR2 and E721 are trademarks of FAAC S.p.A.
This project is not affiliated with or endorsed by FAAC.

---
---

# 🇮🇹 Versione italiana

*[⬆ Back to English](#faac-bus-2easy--protocol-reverse-engineering-and-an-stm32-master)*

Ricostruzione del protocollo proprietario **BUS 2easy** di FAAC: il bus a due fili usato
dalle centrali per cancelli per collegare fotocellule, coste di sicurezza e dispositivi
di comando.

FAAC non pubblica specifiche di questo protocollo. Tutto quello che segue è ricavato da
misure con analizzatore logico e **verificato per implementazione**: un master
ricostruito su STM32G474 genera il bus, alimenta i nodi, li censisce e legge una coppia
di fotocellule e un selettore a chiave FAAC originali.

> ⚠️ **Non usare per impianti reali.** Un impianto in cui i dispositivi di sicurezza sono
> gestiti da elettronica autocostruita esce dalla conformità EN 12453 / EN 12445. Questo
> lavoro è documentazione tecnica e sperimentazione privata.

---

## Livello fisico

Due conduttori, **non polarizzati** lato accessorio: gli accessori hanno un ponte a
diodi in ingresso e non distinguono i due fili. Fanno eccezione gli encoder e il
collegamento fra due centrali, che richiedono polarità.

Lato master la linea è **single-ended**: un filo è massa, l'altro porta alimentazione e
dati insieme.

| Parametro | Valore |
|---|---|
| Tensione di linea | 24 V nominali (13.6 V di media misurata su XBR2, vedi nota) |
| Impedenza di sorgente a riposo | ~12 Ω |
| Protezione | limite di corrente fra 231 e 427 mA, con **rientro automatico** dopo 1–2 s |
| Livello logico basso | 0 V |
| Stato fra i frame | alto |
| Fronti | rampe lineari, ~5 µs, sia in salita sia in discesa |
| Corrente max linea | 500 mA |
| Lunghezza max cavo | 100 m, sezione max 0.5 mm² |
| Consumo tipico nodo | 10 mA per unità |

> Il tester sul bus legge una **media**, non un livello: con il duty di un frame attivo
> la media sta molto sotto il picco. Il livello alto va misurato con l'oscilloscopio.

**Lo stadio di uscita del master è completamente attivo.** Entrambi i fronti sono
pilotati con slew rate controllato: non c'è pull-up né pull-down resistivo. I ~12 Ω di
impedenza di sorgente misurati sono la resistenza d'uscita del regolatore che alimenta
la linea, non un componente. Di conseguenza un master originale dissipa pochi milliwatt
pur alimentando l'intero bus.

### Il master deve restare cedevole

È la proprietà meno ovvia del bus, e sbagliarla fa rifiutare il master dai dispositivi.

Il lato alto deve essere **abbastanza morbido da lasciarsi abbassare da uno slave**. Un
transistor saturo che presenta frazioni di ohm irrigidisce la linea al punto che gli
slave non riescono a segnalare, e segnalano conflitto di indirizzi. Un XBR2 originale
mantiene ~12 Ω di impedenza di sorgente esattamente per questo.

Un master ricostruito ha bisogno di una resistenza in serie — vedi
[stadio di uscita](#stadio-di-uscita-del-master).

### Direzione slave → master

**Gli slave abbassano la linea in tensione.** Il master non rigenera nulla: legge il
livello.

Gli slave assorbono anche un piccolo impulso di corrente mentre rispondono (~2 mA su
~100 µs), ma è la conseguenza del tirare la linea verso massa attraverso l'impedenza del
master, non un canale di segnalazione separato.

---

## Codifica di bit

**Pulse-distance**, auto-sincronizzante. Ogni impulso basso dura ~99 µs. L'informazione
è nella **distanza fra fronti di discesa consecutivi**.

| Simbolo | Distanza fra fronti | Significato |
|---|---|---|
| `0` | 300 µs | slot vuoto |
| `1` | 150 µs + 150 µs | slot occupato |

**Decodifica.** Intervallo > ~220 µs → `0` (consuma 1 slot). Intervallo ≤ ~220 µs → `1`
(consuma 2 intervalli, cioè 1 slot).

| Grandezza | Valore | Dispersione |
|---|---|---|
| Impulso basso | 99 µs | 98–101 µs |
| Slot | **299.876 µs** | ±0.3 µs |
| Frequenza di slot | 3334.7 Hz | |

---

## Struttura del frame

```
│◄──────────────── 20.0 ms ────────────────►│
├──── header ────┬──── dati ────┬─── gap ───┤
│ 8 slot, tutti 1│  55 slot     │  1.05 ms  │
│  slot 0…7      │  slot 8…62   │  (alto)   │
```

Il gap è l'unico intervallo oltre i 500 µs: è l'aggancio di frame.

**Il master non trasmette dati.** L'header è immutabile e nessun bit di dati è mai stato
osservato provenire dal master, nemmeno durante la memorizzazione.

**Nessun checksum.** 63 bit senza parità né CRC, verificato anche con due dispositivi
attivi contemporaneamente. La ridondanza è temporale: 50 ripetizioni al secondo.

### Firme temporali

| Sorgente | gap 1 | gap 2 |
|---|---|---|
| master (header) | 153.1 µs | 148.2 µs |
| XP20B (fotocellula) | 150.1 µs | 151.2 µs |
| XK10B (selettore) | 157.4 µs | 143.9 µs |

Permettono di verificare che un bit provenga dalla famiglia di dispositivo attesa. È
diagnostica, non un meccanismo di sicurezza.

---

## Mappa degli slot

| Slot | Blocco | Formula |
|---|---|---|
| 0–7 | header di sincronismo | — |
| 8–15 | **ignoto** — mai visto attivo | — |
| **16–31** | **fotocellule** | `slot = 16 + DIP` |
| **32–47** | **dispositivi di comando** | `slot = 32 + DIP` |
| 48–63 | **ignoto** — mai visto attivo | — |

Coste ed encoder stanno presumibilmente in 8–15 e 48–63, ma non sono stati misurati.

### Blocco fotocellule (16–31)

| DIP | Slot | Funzione su centrale |
|---|---|---|
| 0000–0011 | 16–19 | sicurezza in apertura |
| 0100–0101 | 20–21 | apertura + chiusura |
| 0110–0111 | 22–23 | sicurezza in apertura |
| 1000–1110 | 24–30 | sicurezza in chiusura |
| 1111 | 31 | impulso OPEN |

**La classe di funzione non sposta lo slot**: dipende solo dal valore numerico dei DIP.

### Blocco dispositivi di comando (32–47)

| Slot | Comando | Slot | Comando |
|---|---|---|---|
| 32 | Open A_1 | 40 | Close |
| 33 | Open A_2 | 41 | Open B_1 |
| 34 | Open A_3 | 42 | Open B_2 |
| 35 | Open A_4 | 43 | Open B_3 |
| 36 | Open A_5 | 44 | Open B_4 |
| 37 | Stop | 45 | Open B_5 |
| 38 | **Stop NC_1** | 46 | non usato |
| 39 | **Stop NC_2** | 47 | non usato |

> ⚠️ **Slot 38 e 39 sono a logica invertita.** Stop NC genera Stop anche a dispositivo
> scollegato: un master conforme deve trattare **bit = 0 come STOP attivo**.

Collegato a un'interfaccia **XBR2/XBR4**, lo stesso spazio si legge come
`slot = 32 + (ID−1)×4 + (Relè−1)`. Il dispositivo riconosce il tipo di master
all'accensione. **Come lo riconosca è ignoto.**

---

## Semantica dei bit

### Fotocellule — fail-safe

Una coppia TX+RX occupa **un solo slot**. Il bit è generato dal ricevitore e richiede il
collegamento ottico stabilito.

| Condizione | Bit |
|---|---|
| Fascio libero, coppia allineata | **1** |
| Fascio interrotto | 0 |
| Disallineamento senza ostacolo | 0 |
| RX o TX scollegato dal bus | 0 |
| Un solo elemento della coppia collegato | 0 |

**Tutti i modi di guasto convergono sullo stato sicuro.** Un master può trattare
**assenza del bit = ostacolo** senza eccezioni. Latenza di transizione: un frame, 20 ms.

Il bus **non distingue** ostacolo da disallineamento.

### Dispositivi di comando

Il bit di comando è **di livello, non impulsivo**. Il rilevamento del fronte è compito
del master.

**Flag di validità.** Accanto al comando, il dispositivo asserisce in permanenza un
secondo bit sullo **slot compagno** (indirizzo XOR 1), se e solo se la propria
configurazione è realizzabile sul master collegato. Il flag ha la firma temporale dello
slave: è il dispositivo a sapere che la sua configurazione è inutilizzabile.

---

## Memorizzazione

**Un dispositivo risponde già prima di essere memorizzato**, in ogni frame. Durante il
SETUP il traffico non cambia di un bit.

Quindi **la memorizzazione è un censimento puramente lato master**: osserva quali slot
sono attivi e li registra come attesi. Da quel momento uno slot atteso che smette di
rispondere è un errore di appello. Lo slave non ha nessuno stato di iscrizione.

Per un master ricostruito, **memorizzare significa salvare la maschera degli slot
attivi**.

> ⚠️ **Il SETUP non autentica.** Un dispositivo con gli stessi DIP produce bit
> indistinguibili da quelli del legittimo. Il protocollo non ha nessuna forma di
> autenticazione: l'unica identità di un nodo è il numero di slot che occupa.

---

## Modalità standby

Una centrale può **disalimentare la linea** e sondarla periodicamente: preambolo
120.27 ms alto, 64 slot con impulso singolo da 47 µs, coda 1.77 ms, poi linea a 0 V per
3.013 s. Ripetizione ogni 3.1545 s, duty 4.5%.

Nessun accessorio può autoalimentarsi in questo stato. Una centrale alimentata in
continua senza rete alternata crede di funzionare a batteria ed entra in questa modalità.

---

## Lettura del bus

```
BUS segnale ──[ R1 ]──┬──── ingresso logico
                       │
                    [ R2 ]
                       │
                      GND ────── comune col negativo del bus
```

| Tensione di linea | R1 | R2 | Uscita |
|---|---|---|---|
| 24 V | 100 kΩ | 15 kΩ | 3.1 V |
| 13.6 V | 100 kΩ | 22 kΩ | 2.5 V |

> **L'impedenza conta più del rapporto.** Se l'ingresso ha un condensatore di filtro —
> tipico degli ingressi digitali industriali — la costante di tempo annulla il segnale.
> Con 10 nF e un partitore da 18 kΩ equivalenti, τ = 180 µs contro impulsi da 99 µs: il
> pin resta fermo al valor medio e non si vede nessun fronte. Usa un partitore a bassa
> impedenza, oppure rimuovi il condensatore.

---

## Stadio di uscita del master

La linea è pilotata da un **PNP high side**. Non viene mai cortocircuitata: per produrre
il livello basso il transistor si spegne e la linea scende da sola.

```
                    +24 V
                      │
          ┌───────────┴───────────┐
          │                       │
       [ R1 ]                 E ──┴──
        22 kΩ                   Q1 BC327   (PNP)
          │                     │
          ├─────────────── B ───┤
          │                     │
       [ R2 ]                 C ──[ R3 ]──┬──────────── BUS +  ──→ nodi
        4.7 kΩ                   100 Ω    │
          │                               │
   drain di Q2                        [ 100 kΩ ]
   (MOSFET N logic-level)                 │
          │                               ├──────────── ingresso MCU
          │                            [ 15 kΩ ]
          │                               │
         GND ──────────────────────────────┴────────── BUS −

   uscita MCU ──[ 100 Ω ]── gate di Q2
   gate ──[ 10 kΩ ]── GND
```

| Rif | Valore | Ruolo |
|---|---|---|
| Q1 | **BC327** (PNP) | 45 V / 800 mA. Evitare componenti da 25 V come l'S8550 |
| Q2 | MOSFET N logic-level | stadio invertente; ≥ 30 V, pochi mA |
| **R2** | **4.7 kΩ** | **limita la corrente di base. Non opzionale** |
| R1 | 22 kΩ | spegne Q1 quando Q2 è interdetto |
| **R3** | **100 Ω** | **mantiene cedevole la sorgente. Non opzionale** |
| partitore | 100 kΩ / 15 kΩ | lettura della linea, **a valle di R3** |

**R2.** Con la base collegata direttamente al drain, la giunzione base-emettitore non ha
nulla in serie quando Q2 conduce: la V_BE clampa a ~0.8 V ma la corrente non è limitata
e la giunzione si distrugge al primo impulso. Con 4.7 kΩ la base sta a ~23.2 V quando
conduce, quindi I_B ≈ 4.9 mA. La saturazione si progetta con β forzato 10–20, non con
quello di datasheet: 49–98 mA garantiti contro i 10–25 mA dei nodi.

**R3.** Senza, il PNP saturo tiene la linea troppo rigida perché uno slave possa
abbassarla, e i dispositivi segnalano conflitto di indirizzi. Conduce solo con Q1 acceso,
quindi dissipa solo quello che assorbe il carico: 25 mA su 100 Ω sono 63 mW.

**Punto di lettura.** Il partitore va **a valle di R3**. A monte legge il collettore, dove
il PNP saturo inchioda la tensione e l'impulso di uno slave è invisibile.

**Scarica della linea.** Quando Q1 si spegne, la corrente dei nodi scarica la linea. Con
pochi nodi e cavo corto basta. Se il fronte di discesa è troppo lento, aggiungi una
resistenza permanente da BUS+ a massa: 2.2 kΩ dissipa 260 mW e non viene mai
cortocircuitata, perché non esiste nessun low-side.

| Uscita MCU | Q2 | Base Q1 | Q1 | Linea |
|---|---|---|---|---|
| **alto** | conduce | tirata giù via R2 | **acceso** | **alta** |
| **basso** | interdetto | tenuta a +24 da R1 | spento | **rilasciata → scende** |

---

## Firmware

Progetto STM32CubeIDE per **STM32G474RCT6** a 170 MHz da quarzo 8 MHz. Contiene sniffer
passivo e master completo, con console su USB CDC. Il master è configurato all'avvio ma
**resta fermo**: pilota il bus solo dopo un comando `M` esplicito.

> **Questo firmware gira su una scheda custom con STM32G474.** Assegnazione dei pin,
> configurazione del clock e messa a riposo delle GPIO in `main.c` sono specifiche di
> quella scheda e **vanno riadattate** all'hardware che si usa. Quello che è portabile è
> la logica di protocollo in `b2e.c` e `b2e_master.c`: temporizzazione, decoder,
> censimento e regole di consenso. Tutto ciò che tocca un pin, un'istanza di timer o un
> canale DMA va verificato sul proprio schema.
>
> Se sposti l'uscita del master su un altro pin, tienila su un timer che possa generare
> l'update e due interrupt di compare. Se sposti l'ingresso di lettura, deve essere
> leggibile come livello GPIO e, per lo sniffer, stare su un canale di input capture.

| Risorsa | Uso |
|---|---|
| PC6 | TIM3_CH1 input capture, fronte di discesa — sniffer e lettura livello per il master |
| TIM3 | base tempi 1 µs (PSC 169) |
| DMA1_Ch3 | cattura circolare, 512 campioni, nessun interrupt |
| PA9 | uscita master, pilota Q2 |
| TIM1 | mezzo slot 150 µs, CC1 = fine impulso, CC2 = istante di lettura |
| USB CDC | console |

Sono i pin della scheda di riferimento: vanno cambiati per adattarli alla propria.

Il master legge il livello della linea da `PC6->IDR`, quindi i due ruoli condividono un
pin: lo sniffer decodifica l'intero frame mentre il master campiona la propria finestra.

### Costanti

`Core/Inc/b2e_master.h`

```c
#define B2EM_HALF_US          150u   /* mezzo slot                       */
#define B2EM_PULSE_US         112u   /* larghezza impulso comandata      */
#define B2EM_SLOTS             64u   /* slot per frame                   */
#define B2EM_GAP_US          1050u   /* gap interframe, linea alta       */
#define B2EM_SAMPLE_US_DEFAULT 30u   /* istante di lettura nello slot    */
```

> **Perché 112 e non 99.** Il valore comandato è il tempo per cui il transistor resta
> spento. La linea impiega qualche microsecondo ad attraversare la soglia del ricevitore,
> e quel tempo viene sottratto all'impulso. Con lo stadio sopra, 112 µs comandati danno
> **99.5 µs misurati** sul bus. Chi replica deve rimisurare: l'offset dipende dal
> percorso di scarica.

L'istante di lettura è dove il master cerca la risposta di uno slave dentro il mezzo slot
dispari. La finestra utile misurata con questo stadio è **20–60 µs**; 30 µs è il centro.

### Censimento e appello

```c
#define B2EM_ENROLL_FRAMES   50u   /* 1 secondo a 50 frame/s               */
#define B2EM_MISSING_LIMIT    5u   /* ~100 ms prima di dichiarare guasto   */
```

`b2em_motion_allowed(photo_mask)` restituisce il consenso al movimento e lo nega in ogni
condizione dubbia: master fermo, censimento non eseguito, errore di appello, slot non
censiti attivi, fotocellula richiesta non libera, slot Stop NC a 0.

Due esclusioni volute:

- **Gli slot 38 e 39 sono esclusi dall'appello.** Con la logica invertita un dispositivo
  presente e a riposo tiene il bit a 0 esattamente come uno scomparso. La loro assenza
  produce comunque lo stato sicuro, quindi l'appello non serve.
- **Il rilevamento intrusi copre solo gli slot 8–31.** I bit di comando sono assenti a
  riposo per natura, quindi non possono essere censiti e produrrebbero un falso positivo
  permanente.

### Console

```
--- sniffer ---
s   stato: statistiche, ultimo frame, dispositivi censiti
f   stampa i frame quando cambiano (default on)
a   stampa tutti i frame (50/s, molto verboso)
e   censimento sniffer: accumula 1 secondo e salva la maschera

--- master (PILOTA IL BUS) ---
M   avvia        X   ferma
m   stato master: risposte, censimento, appello, consenso, livelli grezzi
E   censimento master
B   blink test: commuta la linea a 1 Hz, per le verifiche statiche
+/- sposta l'istante di lettura di 5 µs
```

Il master è configurato all'avvio ma **resta fermo**: pilota il bus solo dopo un `M`
esplicito.

### Messa in servizio

1. **Limite di corrente dell'alimentatore a 300 mA** durante il collaudo.
2. Prova diodi su Q1 prima di montarlo, e verifica col tester quale pin va al +24 V e
   quale a BUS+.
3. `B` e tester su BUS+: deve alternare fra tensione piena e 0 ogni mezzo secondo.
4. Avvia con `M`, poi `m`: regola la finestra con `+`/`-` finché un dispositivo collegato
   non compare in `raw`.
5. `E` per censire, con le fotocellule allineate e libere.
6. Interrompi il fascio: lo slot della fotocellula deve azzerarsi entro un frame e il
   consenso passare a `NO`.

---

## Struttura del repository

```
readme.MD           questo file
firmware/           progetto STM32CubeIDE (STM32G474RCT6)
  Core/Src/b2e.c            sniffer: cattura, decodifica, censimento
  Core/Src/b2e_master.c     master: generazione slot e lettura risposte
  Core/Src/b2e_app.c        console USB CDC
captures/           catture sigrok/PulseView usate per la ricostruzione
```

Le catture sono a 2 MSa/s, canale singolo, tramite partitore passivo.

> sigrok spezza le catture lunghe in `logic-1-1 … logic-1-N`. Vanno concatenati **tutti**
> prima dell'analisi: leggendo solo il primo la cattura sembra vuota.

---

## Stato e questioni aperte

Verificato con FAAC XBR2 (BUS-RELAY), XP20B D (fotocellule), XK10 B (selettore a chiave) e
una centrale E721 in standby.

**Chiuso:** livello fisico, codifica, framing, indirizzamento di fotocellule e dispositivi
di comando, semantica dei bit, comportamento fail-safe, memorizzazione, e un master
funzionante.

**Aperto:**

| Argomento | Impatto |
|---|---|
| Come un dispositivo riconosce il tipo di master (centrale vs XBR2/XBR4) | Alto: cambia l'interpretazione dei DIP dei comandi. Le fotocellule non sono interessate |
| Contenuto degli slot 8–15 e 48–63 | Medio: presumibilmente coste ed encoder |
| Uscita dalla modalità basso consumo | Unico caso noto in cui il master trasmette |
| Transizione fra frame operativo e standby | Medio |
| Soglia esatta della protezione di corrente | Basso: misurata fra 231 e 427 mA |
| Comportamento in caso di conflitto di indirizzi | Basso |

Contributi benvenuti, in particolare misure con dispositivi non disponibili qui.

---

## Licenza

Documentazione e codice rilasciati per uso didattico e di ricerca.
FAAC, BUS 2easy, XP20B, XK10B, XBR2 e E721 sono marchi di FAAC S.p.A.
Questo progetto non è affiliato né approvato da FAAC.
