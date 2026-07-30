# Alexa — Echo Dot 5 setup

The firmware emulates a **Philips Hue bridge** on your LAN. Your Echo discovers
it directly. You need **nothing else**: no hub, no bridge, no smart plug, no
Amazon developer account, no AWS, no subscription, no cloud at all.

It also keeps working with the **internet down** — Echo → ESP32 is a local
network call.

---

## Setup

1. Make sure the ESP32 is on your Wi-Fi (status LED: short flash every 3 s).
2. Say: **"Alexa, discover devices."** Wait ~45 seconds.
3. Eight devices appear, named after your channels ("Socket 1" … "Socket 8").
4. **"Alexa, turn on Socket 3."**

Rename channels in the app or the device's web page and re-run discovery; the
new names are what you then say.

---

## Requirements (all router settings — nothing to buy)

| Requirement | Why |
|---|---|
| Echo and ESP32 on the **same subnet** | Discovery is link-local multicast; it does not cross subnets |
| ESP32 on **2.4 GHz** | ESP32-WROOM has no 5 GHz radio. If your router uses separate SSIDs per band, put the Echo on 2.4 GHz too, or bridge the bands into one subnet |
| **AP/client isolation OFF** | Isolation blocks device-to-device traffic, so the Echo cannot reach the ESP32 |
| **Multicast/SSDP not filtered** | Some mesh systems (Deco, Google Wifi, several ISP routers) drop SSDP by default |

These three are the cause of nearly every "Alexa can't find my device".

---

## How it works

```
Echo ──[1] SSDP M-SEARCH ──▶ 239.255.255.250:1900
Echo ◀─[2] "I'm a Hue bridge, see http://<ip>/description.xml" ── ESP32
Echo ──[3] GET /description.xml ─────────────────▶ ESP32
Echo ──[4] POST /api  (link button) ─────────────▶ ESP32   (always accepted)
Echo ──[5] GET /api/<user>/lights ───────────────▶ ESP32   (8 channels)
Echo ──[6] PUT /api/<user>/lights/3/state {"on":true} ──▶ ESP32 → relay clicks
```

Steps 3–6 share port 80 with the device's own web UI. Hue paths look like
`/api/<username>/…` and never collide with the firmware's own fixed `/api/<name>`
routes.

---

## Diagnosing a failed discovery

Watch the serial monitor (`pio device monitor`) while you say "discover devices":

| What you see | Meaning |
|---|---|
| `SSDP M-SEARCH from 192.168.x.x (discovery #1)` | The Echo reached you. Good. |
| `description.xml served to …` | The Echo is interested. |
| `light list served (8 channels)` | Discovery succeeded — the devices will appear. |
| **Nothing at all** | The M-SEARCH never arrived: wrong subnet, band split, AP isolation, or SSDP filtering. |
| `SSDP multicast listen failed` | The ESP32 could not bind the multicast socket — usually Wi-Fi was not up yet. It retries on reconnect. |

Then, when you give a voice command:

```
[alexa] voice: Living Room Fan -> ON
```

---

## If discovery still fails: present as lights instead

Modern Echo firmware handles `On/Off plug-in unit` correctly, which is what
sockets actually are, and Alexa then shows them as plugs. A few older Echo
firmwares only discover *lights*.

To switch, set `alexaAsPlug` to `false`:

```bash
curl -X POST http://smarthome-XXXX.local/api/config \
     -H 'Content-Type: application/json' \
     -d '{"device":{"alexaAsPlug":false}}'
```

Then in the Alexa app **remove the old devices** and run discovery again. They
now appear as dimmable lights. Brightness is ignored beyond on/off — any level
above 0 % turns the socket on.

---

## Naming that works

- Give every channel a **distinct** name. Two sockets called "Light" is
  ambiguous and Alexa will pick one.
- Avoid names that collide with Alexa's own vocabulary ("Alexa", "Computer",
  "Echo", "Music", "Everything").
- Short and phonetic wins: "Porch", "Study Fan", "Water Pump" beat
  "Socket number three near the window".
- After renaming, run discovery again — Alexa caches the old names.

---

## Limits of the local bridge

| Works | Does not work |
|---|---|
| Voice on/off at home | Voice from outside the house |
| Works with the internet down | Alexa routines that need cloud state |
| Alexa groups and rooms | Brightness/colour (these are relays) |
| Instant response, no cloud round trip | Energy reporting |

Voice control from outside the house needs the **cloud Alexa Smart Home Skill**
— that's Phase 4, and it needs an Amazon developer account, an AWS Lambda, OAuth
account linking, and a public HTTPS endpoint. At home, the local bridge is
strictly better: faster, free, and offline-tolerant.

---

## Turning it off

```bash
curl -X POST http://smarthome-XXXX.local/api/config \
     -H 'Content-Type: application/json' \
     -d '{"device":{"alexaEnabled":false}}'
```

This stops the SSDP responder and the Hue endpoints. Reboot the ESP32 and remove
the devices from the Alexa app.
