# Wiring Guide — ESP32 DevKit V1 + 8-channel relay + mains sockets

> **Read this before connecting anything to mains.** Three of the steps here are
> not optional preferences — skipping them either destroys the board, makes the
> device unreliable in a way that looks like a firmware bug, or is dangerous.

---

## 0. What you have

| Item | Detail |
|---|---|
| MCU | ESP32 DevKit V1, 30-pin, USB-C, ESP32-WROOM-32, 4 MB flash |
| Relay board | 8-channel, SONGLE **SRD-05VDC-SL-C**, opto-isolated, **active LOW** |
| Jumper | **VCC ↔ JD-VCC**, fitted from the factory — **must be removed** |
| Supply | 5 V, **2 A minimum** (see §2) |
| Load | Indian modular wall board, 6 A female sockets |

---

## 1. The three non-negotiable steps

### 1.1 Fit 10 kΩ pull-ups on all eight relay IN lines

```
        3.3 V (ESP32)
          │
        [10 kΩ]
          │
GPIO ─────┴───── IN(n) on the relay board
```

**Why:** during reset, and for the ~250 ms before `setup()` runs, every ESP32
GPIO is a floating input. An active-LOW relay board reads "floating" as ON.
Without pull-ups, **all eight sockets switch on for about a third of a second
on every single boot and every brownout.**

Firmware cannot fix this — the code is not running yet. `RelayManager::begin()`
drives the output latch before switching the pin to an output, which is the most
software can do, but the pull-ups are what actually hold the coils off.

Eight resistors. Do not skip them.

### 1.2 Remove the JD-VCC jumper and give the coils their own supply

The yellow jumper ties the relay coil supply to the ESP32's 5 V rail. Eight
coils at ~70 mA each is **~560 mA**, on a rail that also feeds a Wi-Fi radio
peaking around **500 mA**. A USB port cannot do this.

The symptom is *not* "the relays don't work". The symptom is random reboots that
look exactly like a firmware crash. The firmware detects and reports this
explicitly — check `docs`/the diagnostics page for `resetReason: brownout`.

```
   ┌──────────── 3-pin power header ────────────┐
   │   GND        VCC        JD-VCC             │
   │    │          │           │                │
   │    │          │           └── 5 V + from the 2 A supply
   │    │          └── ESP32 VIN (5 V)   ← opto LEDs only, ~4 mA/channel
   │    └── ESP32 GND  AND  5 V − from the 2 A supply
   └────────────────────────────────────────────┘
                  ↑ jumper REMOVED
```

> **Honest caveat:** on these boards the two GND pins are the same copper net,
> so removing the jumper does **not** give true galvanic isolation. What it does
> give — and what actually fixes your problem — is moving the coil current off
> the ESP32's regulator.

Power the ESP32 from the same 2 A supply via **VIN**, not from a phone charger,
once it leaves the bench.

### 1.3 Wall switches must be dry contacts — never mains

The switches on your existing wall board currently carry **230 V**. Connecting
one to a GPIO in that state puts mains on the ESP32: lethal, and it destroys the
board instantly.

Each switch must be **completely disconnected from mains** and rewired as an
isolated contact:

```
Switch terminal A ────────── ESP32 GPIO   (configured INPUT_PULLUP)
Switch terminal B ────────── ESP32 GND
```

Both wires then carry **3.3 V and microamps**. Nothing else.

If you would rather not rewire them yet, run app-only for now: set
`switchEnabled: false` for every channel in the config, or simply leave the
inputs unconnected — the internal pull-ups hold them firmly HIGH and the
firmware will never see a phantom press.

---

## 2. Power budget

| Consumer | Typical | Peak |
|---|---|---|
| ESP32 (Wi-Fi TX) | 120 mA | **500 mA** |
| One relay coil | 70 mA | 80 mA |
| Eight coils | 560 mA | 640 mA |
| **Total** | ~700 mA | **~1.15 A** |

Use a **5 V / 2 A** supply. A 1 A supply will brown out under simultaneous Wi-Fi
transmit and a multi-relay scene, which is exactly when it is hardest to
diagnose. Add a **1000 µF** electrolytic across the 5 V rail near the relay
board to absorb coil inrush.

The firmware staggers gang operations by 40 ms per channel
(`kRelayGangStaggerMs`) so eight coils never inrush on the same millisecond —
but that mitigates the peak, it does not replace an adequate supply.

---

## 3. Pin map

### Relay outputs (active LOW — pin LOW = relay ON)

| Ch | GPIO | Board label | | Ch | GPIO | Board label |
|----|------|-------------|-|----|------|-------------|
| R1 | 23 | D23 | | R5 | 18 | D18 |
| R2 | 22 | D22 | | R6 | 5  | D5  |
| R3 | 21 | D21 | | R7 | 17 | TX2 |
| R4 | 19 | D19 | | R8 | 16 | RX2 |

Chosen to avoid the strapping pins that boot LOW (GPIO 0, 2, 12, 15) — on an
active-LOW board, LOW at boot means *relay energised*. Using TX2/RX2 costs you
`Serial2`, which this firmware does not use.

### Switch inputs (`INPUT_PULLUP`; the closed contact pulls to GND)

| Sw | GPIO | | Sw | GPIO |
|----|------|-|----|------|
| S1 | 13 | | S5 | 25 |
| S2 | 14 | | S6 | 33 |
| S3 | 27 | | S7 | 32 |
| S4 | 26 | | S8 | 4  |

All eight support an internal pull-up. (GPIO 34–39 do **not**, which is why they
are unused.)

### Other

| Function | Pin | Notes |
|---|---|---|
| Status LED | GPIO 2 | Onboard blue LED — blink codes below |
| Factory reset | GPIO 0 | The BOOT button. Hold **10 s**. |

### LED blink codes

| Pattern | Meaning |
|---|---|
| Solid | Booting |
| Fast blink (120 ms) | Provisioning — SoftAP up, waiting for Wi-Fi credentials |
| Slow blink (500 ms) | Connecting to Wi-Fi |
| Single short flash every 3 s | Online, local only |
| Double flash every 3 s | Online, cloud connected |
| Rapid triple blink | Fault — heap critical |
| Continuous strobe | OTA update in progress |

---

## 4. Mains wiring, per socket

```
                    ┌─────────────────────────────────┐
  MCB ── LIVE ──────┤ COM        Relay n          NO  ├────── Socket L
                    └─────────────────────────────────┘
         NEUTRAL ─────────────────────────────────────────── Socket N
         EARTH ───────────────────────────────────────────── Socket E
```

**Use NO (normally open), never NC.** With NO, an unpowered or rebooting board
leaves every socket **OFF**. With NC it would leave them all **ON** — the
opposite of what you want during a fault.

Neutral and Earth go straight through and are **never switched**.

### Current limits — the important derating

The relays are marked *10 A 250 VAC*. That is a resistive-load, single-relay,
ideal-conditions number. On a shared 8-channel PCB with ~2 mm traces:

- **Derate to ~5 A per channel (≈1200 W).**
- A 6 A Indian socket at full load is already 1400 W — do not run one at its
  rating through this board.
- **Never switch a 16 A socket** (AC, geyser, pump). That needs a **contactor**
  driven by the relay, not the relay itself.
- Avoid loading many channels heavily at the same time.

### Inductive loads will eventually weld the contacts

Fridges, fans, pumps and motors draw 5–8× inrush and arc on break. The contacts
pit and eventually stick **closed** — a socket you can no longer switch off.

For any motor load, fit an RC snubber across the contacts:

```
   COM ──┬── 100 Ω ── 0.1 µF (X2 rated) ──┬── NO
         └────────── load path ───────────┘
```

Or drive a proper contactor.

### Wire and enclosure

- Mains connections go into the relay's **screw terminals**, using **1.5 mm²**
  house wire. **Never DuPont jumper wires** — they have no insulation rating for
  230 V and are a fire risk.
- DuPont jumpers are fine for the ESP32 ↔ relay IN signals, and nowhere else.
- Everything goes in an enclosed, non-conductive box.
- Keep mains wiring physically separated from the ESP32 and signal wiring.
- The circuit must be behind an MCB and, ideally, an RCD.

---

## 5. Bench test before touching the wall board

Do this first, every time:

1. Wire the ESP32 to the relay board only — **no mains at all**.
2. Power up. Confirm **no relay clicks during boot** (this validates §1.1).
3. Power-cycle 10 times. Still no clicks.
4. Flash the firmware, open the serial monitor at 115200:
   ```
   pio run -e esp32dev -t upload -t monitor
   ```
5. Connect to the `SmartHome-XXXX` AP (password printed in the serial log),
   open `http://192.168.4.1`, and toggle each channel. Listen for one click per
   toggle, on the right relay.
6. Only then wire a single lamp through **one** relay and test it.
7. Only then move to the wall board.

---

## 6. Assembly checklist

- [ ] 8 × 10 kΩ pull-ups fitted, relay IN → 3.3 V
- [ ] JD-VCC jumper **removed**
- [ ] 5 V / 2 A supply on JD-VCC, sharing GND with the ESP32
- [ ] ESP32 powered from VIN off the same supply
- [ ] 1000 µF cap across the 5 V rail near the relay board
- [ ] All 8 relay signal lines on the GPIOs in §3
- [ ] Wall switches fully disconnected from mains, rewired as dry contacts
- [ ] Mains on **NO** contacts, neutral and earth unswitched
- [ ] 1.5 mm² wire in screw terminals; no DuPont anywhere near mains
- [ ] No load above ~5 A on any channel; no 16 A sockets
- [ ] Snubbers on any motor loads
- [ ] Everything enclosed; circuit behind an MCB
- [ ] Bench tested with no mains, then with one lamp

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| All relays click on every boot | No pull-ups | §1.1 |
| Random reboots, `resetReason: brownout` | Coils on the ESP32 rail, or 1 A supply | §1.2 |
| Reboots only when several relays switch together | Supply too small | 2 A + bulk cap |
| Relay clicks but the socket stays dead | Wired to NC instead of NO | Move to NO |
| Socket cannot be turned off any more | Welded contacts from an inductive load | Replace the relay, add a snubber |
| A switch toggles by itself | Switch input picking up mains coupling | Separate the wiring; the switch must be a dry contact |
| App toggles work, wall switch does nothing | `switchEnabled: false`, or switch not wired to GND | Check config and wiring |
| First flip after an app command does nothing | Should not happen — the firmware toggles relative to relay state, not switch position | Report it |
| Echo will not discover the device | 5 GHz SSID, AP isolation, or SSDP filtering | See `docs/ALEXA.md` |
