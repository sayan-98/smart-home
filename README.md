# Smart Home OS

An ESP32-based smart home system that switches eight mains sockets from **three
places at once** — the wall switch, a tablet app, and Alexa — and keeps the
on/off state correct everywhere, no matter which one changed it.

Built to run entirely on **free hosting**, and to keep working with the
**internet down**.

```
┌──────────────┐   ┌──────────────┐   ┌──────────────┐
│ Wall switch  │   │  App/tablet  │   │  Echo Dot 5  │
└──────┬───────┘   └──────┬───────┘   └──────┬───────┘
       │ <2 ms            │ LAN or cloud     │ local Hue emulation
       └──────────────────┴──────────────────┘
                          ▼
              ┌───────────────────────┐
              │  ESP32 · RelayManager │  ← single writer, monotonic revision
              └───────────┬───────────┘
                          ▼
                  8 × relay → socket
```

---

## What's here

| Directory | What it is | Status |
|---|---|---|
| [`firmware/`](firmware/) | ESP32 firmware (PlatformIO, C++17, FreeRTOS) | Builds: RAM 20.4%, Flash 84.6% |
| [`backend/`](backend/) | Node + Express + Socket.IO + MQTT bridge + Groq AI | Typechecks clean |
| [`app/`](app/) | Capacitor app, tablet-first (Redmi Pad 2) | Builds: 83 KB gzipped |
| [`infra/`](infra/) | docker-compose for local dev, Render blueprint | — |
| [`docs/`](docs/) | Wiring, Alexa, MQTT, REST API, OTA, architecture | — |

---

## Read this first

Three wiring steps are **not optional** — skipping them either destroys the
board or causes faults that look like software bugs. Full detail in
[`docs/WIRING.md`](docs/WIRING.md):

1. **Fit 10 kΩ pull-ups on all eight relay IN lines.** Without them every socket
   switches on for ~300 ms on each boot and brownout. Firmware cannot fix this —
   the code isn't running yet.
2. **Remove the JD-VCC jumper**, give the relay coils their own 5 V / 2 A supply.
   Eight coils draw ~560 mA; a USB port cannot do it. The symptom is random
   reboots that look exactly like a crash.
3. **Never wire a mains switch to a GPIO.** Switch inputs must be dry contacts
   (GPIO ↔ GND, 3.3 V only).

Also: derate the relays to **~5 A per channel**, use **NO** contacts (so an
unpowered board leaves sockets off), and put a snubber on any motor load.

---

## The common setup: one master wall switch

If you have a single manual switch feeding the whole board and want each socket
individually controllable by app and Alexa — no new switches:

```
MCB ──▶ [existing manual switch] ──┬──▶ 5 V supply ──▶ ESP32 + relay coils
                                   └──▶ Relay COM ──▶ NO ──▶ each socket L
```

The switch becomes a master kill switch. Each socket gets its own relay. GPIO
switch inputs stay unused — with internal pull-ups they idle high and never fire.

Set each channel's restore mode to **`last`** so flipping the master back on
returns the sockets to how you left them. Keep heating elements on `off`.

| You do | What happens |
|---|---|
| Master switch off | Everything dies, including the ESP32 |
| Master switch on | Sockets restore in ~1 s; Wi-Fi and Alexa in ~10 s |
| Tap a tile in the app | That socket switches; Alexa's view updates |
| "Alexa, turn on Fan" | That socket switches; the app tile updates |

---

## Quick start

### Firmware

```bash
cd firmware
pio run -e esp32dev                    # build
pio run -e esp32dev -t upload -t monitor
pio test -e native                     # 13 logic tests
```

First boot raises a WPA2 SoftAP `SmartHome-XXXX` (password in the serial log).
Connect, open `http://192.168.4.1`, choose your Wi-Fi, rename the sockets. That
page is also a permanent control panel at `http://smarthome-XXXX.local`.

### Alexa

Say **"Alexa, discover devices."** Eight sockets appear. Nothing to buy, no
account, no cloud — it works with the internet down. The Echo and ESP32 must be
on the same 2.4 GHz network with AP isolation off. See
[`docs/ALEXA.md`](docs/ALEXA.md).

### Backend

```bash
cd backend
cp .env.example .env      # fill in Supabase, HiveMQ, and optionally Groq
npm install && npx prisma db push
npm run dev
```

Local development without any cloud account:

```bash
docker compose -f infra/docker-compose.yml up -d   # Postgres + Mosquitto
```

### App

```bash
cd app
npm install
npm run build
npx cap add android
npm run android:run       # deploys to a connected tablet
```

Needs **JDK 17** and Android Studio. On Redmi/HyperOS, enable both *USB
debugging* **and** *Install via USB* in Developer Options.

---

## Design decisions worth knowing

**One writer.** Three actors mutating shared state is a distributed-systems
problem in a hardware costume. `RelayManager` on the device is the only thing
that touches a GPIO. Every change carries its `source` and a monotonic `rev`, so
the app can say *"Porch turned on — wall switch, 2 seconds ago"*, and a delayed
cloud command can never undo a wall-switch press that happened after it was sent.

**Offline-first is literal.** With the internet down: wall switches work, the app
works over LAN, Alexa works, schedules fire, automations run, and state restores
after a power cut. Only remote access, cloud sync, OTA and AI stop.

**The physical switch always wins.** Its path never touches the network —
guaranteed under 20 ms, typically ~2 ms.

**AI is never in the switching path.** Groq is used to *author* rules and to
*interpret* one-off sentences. The rules then run on the device, offline,
forever. The model proposes a JSON action list; the backend re-validates every
action against the real database and the caller's real permissions before
anything reaches a relay.

**Free tier, honestly.** Render can't host MQTT (no raw TCP, and it sleeps), so
the broker is HiveMQ Cloud — which is TLS-only, making TLS mandatory rather than
optional. Supabase free is 500 MB and a 30 s heartbeat is ~1 M rows/device/year,
so telemetry retention is a load-bearing feature, not a nicety.

**Real OTA rollback.** Two 1.875 MB slots. A new image is marked valid only after
it boots *and* gets online for 45 seconds. Marking it valid just because
`setup()` ran would miss the exact failure that matters — an image that boots
fine but can't reach the network.

---

## Known limits

- **No RTC.** After a power cut with no internet the device doesn't know the
  time, and the scheduler refuses to fire rather than guess. Add a DS3231 if that
  matters.
- **NVS is not encrypted.** Anyone with the board and a USB cable can read the
  Wi-Fi password and API key. Flash encryption + secure boot are irreversible per
  chip and deliberately deferred. Treat the device key as *revocable*, not secret.
- **The LAN API is plain HTTP.** Anyone already on your network can read it.
- **Zigbee/Thread are impossible on this chip** — no 802.15.4 radio. That needs
  an ESP32-C6 or -H2. Silicon, not software.

39 further gaps and contradictions found in the original requirements are
documented in the planning notes, covering hosting limits, the security model,
and hardware safety.

---

## Documentation

| Doc | Covers |
|---|---|
| [WIRING.md](docs/WIRING.md) | Pin map, power budget, mains safety, checklist, troubleshooting |
| [ALEXA.md](docs/ALEXA.md) | Echo setup, why discovery fails, naming |
| [API.md](docs/API.md) | On-device REST + WebSocket |
| [MQTT.md](docs/MQTT.md) | Topic tree, conflict resolution, ACLs |
| [OTA.md](docs/OTA.md) | Staged rollout, rollback testing, cert expiry |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Task table, state model, heap budget |
