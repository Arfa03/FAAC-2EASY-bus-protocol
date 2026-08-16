# Stadio 2 — master BUS-2EASY

**Questo modulo pilota il bus.** Non avviarlo finché lo sniffer non ha girato
un'ora sopra il 99.9% con overrun a zero.

Il master è configurato all'avvio ma resta **fermo**. Si accende solo dal comando
`M` sulla console.

---

## Cablaggio

XBR2 **completamente scollegato**.

```
  13.6V banco ──[100Ω 2W]──┬─────────────────── BUS+ ──── fotocellula
                            │
                    drain IRLZ44N (uscita "Iniettore", PA9)
                            │
                        [100k]
                            ├────────────────── PC6
                         [22k]
                            │
  slave BUS− ──[10Ω]────────┴──── GND ──── negativo alimentatore
                   │
                   └── PA3
```

**Limite di corrente dell'alimentatore a 300 mA.** È la protezione mentre si
collauda: un bug che lascia il MOSFET acceso mette in corto il 100 Ω e basta.

Verifica prima di dare tensione che il diodo di ricircolo dell'uscita iniettore
abbia il catodo su Vin. Se conducesse, la linea non salirebbe mai.

Dissipazione del 100 Ω: 1.85 W a linea bassa, ~0.6 W medi al 33% di duty.
Serve un 2 W, o due da 220 Ω in parallelo.

Caduta a riposo con 15 mA di nodi: 1.5 V, quindi linea a ~12.1 V. Gli XP20B
funzionano (la E721 lavorava a 12 V), ma controlla DL2 sull'RX: deve dire
"collegamento presente".

---

## Come funziona

Si ragiona a **mezzi slot da 150 µs**, non a slot da 300:

- mezzo slot **pari** → impulso sempre presente. È il clock del bus.
- mezzo slot **dispari** → impulso presente solo se il bit vale 1.

Uno slot = 300 µs = due mezzi slot. Un frame = 64 slot + gap da 1.05 ms = 20 ms.

L'ADC è triggerato da `TIM1_CH3`, che non esce su nessun pin e serve solo a
posizionare la **finestra di campionamento** dentro il mezzo slot. Se la corrente
letta supera la soglia, il mezzo slot dispari successivo riceve l'impulso: è la
rigenerazione del bit.

---

## Console

```
M   avvia il master        X   ferma
K   calibra (misura il riposo e imposta la soglia)
m   stato: baseline, soglia, risposte, profilo dei campioni grezzi
+/- sposta la finestra di campionamento di 5 µs
>/< alza o abbassa il margine di soglia di 10 conteggi
```

---

## Taratura

La finestra e la soglia sono i due parametri da trovare sul campo. Non li
conosciamo: sappiamo solo che il master originale rigenera l'impulso a
**150–157 µs** dall'inizio dello slot, quindi lo slave deve assorbire **prima**.
La finestra utile è fra 100 e 150 µs; il default è 125.

**Procedura:**

1. Collega **solo l'alimentazione**, nessuno slave. Comando `M`, poi `K`.
   La baseline deve essere bassa e stabile.
2. Collega la fotocellula, fascio libero. Comando `m` e guarda il profilo `raw`:
   lo slot 16 deve avere un valore **più alto** degli altri. Se non si distingue,
   sposta la finestra con `+` e `-` finché non emerge.
3. Con la finestra buona, ricalibra con `K` a fotocellula scollegata, poi
   ricollega. Regola il margine con `>` e `<` finché lo slot 16 compare fra le
   risposte e nessun altro.
4. Interrompi il fascio: lo slot 16 deve sparire.

Se il profilo `raw` è piatto in ogni posizione della finestra, lo slave non si sta
agganciando al tuo frame. Vedi sotto.

---

## Se gli slave non rispondono

È il rischio principale, e non sappiamo come gli slave si sincronizzino: se sul
gap, sull'header, o su entrambi. Cose da provare, in ordine:

1. **Controlla il LED DL2 dell'RX.** Se dice "collegamento assente", lo slave non
   riconosce il bus come valido. Se dice "connessione presente", si è agganciato e
   il problema è nella finestra di campionamento.
2. **Verifica la forma d'onda** con lo sniffer stesso: PC6 legge quello che il tuo
   master emette. Devi decodificare `11111111` seguito da zeri. Se non lo fa, il
   problema è nella generazione, non negli slave.
3. **Tensione di linea.** A 12.1 V dovrebbero funzionare, ma se sono al limite
   abbassa il pull-up a 47 Ω (attenzione: 3.7 W a linea bassa, serve dissipazione).
4. **Geometria del gap.** 1.05 ms è il valore misurato sull'XBR2. Se gli slave non
   si agganciano, è il primo parametro da variare.

---

## Cosa non fa ancora

- Non gestisce il limite di corrente né il rientro automatico: quello lo fa
  l'alimentatore da banco.
- Non ha lo slew rate controllato dei fronti (il master originale ha rampe da
  5 µs). Su cavo corto non serve; su 100 m sì.
- Non distingue le firme temporali delle famiglie di dispositivo.
- Il campionamento è a un solo punto per slot. Per caratterizzare la forma
  dell'impulso di corrente servirebbe un profilo a più punti.
