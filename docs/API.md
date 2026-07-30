# On-device REST + WebSocket API

Served by the ESP32 itself on port 80. This is what makes offline-first real:
with the internet down the router still routes, so the app — or any browser —
reaches `http://smarthome-XXXX.local` and has complete control.

`XXXX` is the last 4 hex digits of the MAC, also shown in the serial log and on
the SoftAP SSID.

---

## Authentication

| Device state | Reads | Writes |
|---|---|---|
| **Unclaimed** | open on LAN | open on LAN |
| **Claimed** | open on LAN | `X-API-Key: <device key>` **or** a session cookie |

An unclaimed device has to be reachable — that is how it gets set up, and it
holds nothing worth stealing yet.

To get a session cookie (this is what the built-in web page uses), exchange the
**claim code** printed on the enclosure:

```http
POST /api/local-auth
{ "code": "K7QM3XPA" }
→ 200  Set-Cookie: sh_session=<token>; Max-Age=43200
```

Wrong codes are answered after a deliberate 750 ms delay — it is the only
brute-forceable secret on the device.

> **Limitation, stated plainly:** this is plain HTTP on your LAN. Anyone already
> inside your network can read the traffic. TLS on the LAN path is Phase 4.

---

## Endpoints

### Device

| Method | Path | Auth | Description |
|---|---|:--:|---|
| GET | `/` | — | The built-in web UI (control, Wi-Fi, settings, diagnostics) |
| GET | `/api/info` | — | Identity, link state, claim state, clock |
| GET | `/api/diag` | — | Full diagnostics + the recent log ring |
| POST | `/api/reboot` | ✔ | Flush state, then restart |
| POST | `/api/factory-reset` | — | Body must contain the claim code |

`GET /api/info`:

```json
{
  "uuid": "sh-a4cf12b9e0d4", "mac": "A4:CF:12:B9:E0:D4",
  "name": "Smart Home Node", "firmware": "1.0.0", "relayCount": 8,
  "hostname": "smarthome-e0d4",
  "wifiConnected": true, "apActive": false, "ssid": "Home", "rssi": -54,
  "ip": "192.168.1.42",
  "claimed": false, "claimCode": "K7QM3XPA",
  "timezone": "IST-5:30", "timeSynced": true,
  "time": "2026-07-30T23:14:05+05:30"
}
```

`claimCode` is only exposed while the device is **unclaimed**. Afterwards it is
the local-auth password and is withheld.

### Relays

| Method | Path | Auth | Description |
|---|---|:--:|---|
| GET | `/api/state` | — | Snapshot of all channels |
| POST | `/api/relay/<0-7>` | ✔ | Control one channel |
| POST | `/api/relay/all` | ✔ | Control every channel |
| POST | `/api/relay/group` | ✔ | Control a group (`{"group":1,…}`) |

Request body:

```json
{ "action": "toggle" }              // on | off | toggle
{ "state": true }                   // equivalent to action:"on"
{ "action": "on", "rev": 1044 }     // applied only if newer — see MQTT.md
{ "seconds": 600 }                  // on, auto-off after 10 minutes
```

`GET /api/state`:

```json
{
  "rev": 1043, "mask": 5,
  "channels": [
    { "channel": 0, "name": "Porch", "icon": "socket", "state": true,
      "source": "physical", "rev": 1043, "changedAt": 1785432101234,
      "tsSynced": true, "group": 0, "enabled": true, "autoOffInSec": 0 }
  ]
}
```

### Configuration

| Method | Path | Auth |
|---|---|:--:|
| GET | `/api/config` | — |
| POST | `/api/config` | ✔ |

`POST` accepts a **partial patch**; anything omitted is left alone. The whole
patch is validated before any of it is applied, so a rejected request leaves the
running configuration untouched.

```json
{
  "device": { "name": "Living Room", "timezone": "IST-5:30",
              "alexaEnabled": true, "alexaAsPlug": true, "logLevel": 3 },
  "channels": [
    { "index": 0, "name": "Porch", "restore": "off",
      "switchEnabled": true, "switchMode": "latching",
      "debounceMs": 25, "longPressMs": 0, "doublePressMs": 0,
      "group": 1, "autoOffSec": 0 }
  ]
}
```

`mqttPass` reads back as `********` and sending that literal value back never
overwrites the stored password.

Per-channel fields worth knowing:

- `restore` — `off` (default, safest for sockets) | `on` | `last`
- `switchMode` — `latching` (Indian modular rockers) | `momentary` (push buttons)
- `longPressMs` / `doublePressMs` — **0 disables the gesture and keeps the short
  press instant.** Enabling either necessarily defers the short press to the
  release edge; that cost is paid only by channels that opt in.
- `longPressAction` / `doublePressAction` — `none | toggle | all_off | all_on |
  group_toggle`

### Wi-Fi

| Method | Path | Auth | Notes |
|---|---|:--:|---|
| GET | `/api/wifi/scan` | — | Cached results + a `scanning` flag |
| GET | `/api/wifi/saved` | — | Saved SSIDs (never the passwords) |
| POST | `/api/wifi` | ✔* | `{"ssid":"…","password":"…"}` |
| POST | `/api/wifi/forget` | ✔ | `{"ssid":"…"}` |

\* Open while the recovery SoftAP is up — otherwise a device that lost its
network could never be re-adopted.

Scanning takes seconds and must not run on the HTTP task, so it happens on the
Wi-Fi task and this endpoint returns the cache:

```json
{ "scanning": true, "ageMs": 800,
  "networks": [ { "ssid": "Home", "rssi": -52, "channel": 6,
                  "secure": true, "known": true } ] }
```

Poll until `scanning` is false. Up to 5 networks are stored; the device scans
and joins the **strongest** one it knows.

### Schedules and automations

| Method | Path | Auth |
|---|---|:--:|
| GET / POST | `/api/schedules` | GET —, POST ✔ |
| GET / POST | `/api/automations` | GET —, POST ✔ |

`POST` replaces the whole set, validated before anything is stored. Both run
entirely on the device — see `ARCHITECTURE.md`.

Schedule:

```json
[{ "id": "morning", "enabled": true, "channels": [0,3], "action": "on",
   "minute": 420, "days": 127, "catchUp": true }]
```

`minute` = minutes since local midnight. `days` = bitmask, bit 0 = Sunday.
`catchUp` fires a rule late if the device was off across its time today.

Automation:

```json
[{ "id": "porch-with-gate", "enabled": true, "cooldownSec": 5,
   "trigger":   { "type": "relay", "channel": 0, "state": true },
   "conditions":[{ "type": "timeBetween", "from": 1080, "to": 360 }],
   "actions":   [{ "type": "relay", "channels": [3], "action": "on" }] }]
```

Triggers: `relay | switchShort | switchLong | switchDouble | boot | online |
offline`. Conditions: `channel | timeBetween | anyOn | allOff`. Actions:
`relay | timer | allOff | allOn | group`, each with an optional `delaySec`.

### Claim and OTA

| Method | Path | Description |
|---|---|---|
| POST | `/api/claim` | `{"claimCode","apiKey","homeId","roomId","apiBase","mqttHost",…}` |
| POST | `/api/local-auth` | `{"code":"<claim code>"}` → session cookie |
| GET | `/api/ota/status` | State, progress %, last error, running partition |
| POST | `/api/ota/check` | `{}` to check the manifest, or `{"url","sha256"}` |

---

## WebSocket — `ws://<host>/ws`

The live path. REST polling is only a fallback.

**Server → client**

```json
{ "type": "hello",  "uuid": "sh-…", "fw": "1.0.0", "channels": 8 }
{ "type": "state",  "data": { …the /api/state payload… } }
{ "type": "relay",  "channel": 2, "state": true, "source": "physical", "rev": 1044 }
```

A `state` snapshot is pushed on connect and after any config, Wi-Fi or clock
change. Individual `relay` messages carry every subsequent change — including
ones caused by a wall switch, Alexa, a schedule or an automation.

**Client → server**

```json
{ "cmd": "set", "channel": 2, "action": "toggle", "key": "<api key>" }
{ "cmd": "all", "action": "off", "key": "<api key>" }
```

On a claimed device, commands need `key` (the device API key) or `session` (the
token from `/api/local-auth`). Without one the socket is read-only.

---

## Errors

```json
{ "error": "channel 0 debounceMs out of range" }
```

| Code | Meaning |
|---|---|
| 400 | Validation failed — the message says which field |
| 401 | Authentication required |
| 403 | Wrong claim code |
| 404 | No such channel or route |
| 409 | An OTA is already running |
| 500 | Out of memory (check `/api/diag` → `memory.freeHeap`) |

---

## Discovery from the app

mDNS: `_http._tcp` on `smarthome-XXXX.local`, with TXT records `uuid`, `fw` and
`ch` (channel count). The Capacitor app uses this to detect that it is on the
same LAN as the device and switch from cloud to direct mode automatically.
