# MQTT Topic Guide

Client: **ESP-IDF native `esp_mqtt`** — not PubSubClient, which cannot publish
above QoS 0 and would have silently downgraded every state report.

- **QoS 1** both directions
- **TLS** via the bundled Mozilla root store (no pinned CA that can expire)
- **LWT** so the backend knows within `keepalive` that a device dropped
- **Retained** state topics, so a client that connects three days later gets the
  truth immediately with no round trip to the device

---

## Base topic

```
smarthome/<homeId>/<deviceUuid>
```

`homeId` is `unclaimed` until the device is adopted. `deviceUuid` is
`sh-<mac>` — derived deterministically from the eFuse MAC, so it survives a
factory reset and a reflash.

Example: `smarthome/home_7f3a/sh-a4cf12b9e0d4`

---

## Device → broker

| Topic | QoS | Retain | Payload |
|---|:--:|:--:|---|
| `<base>/status` | 1 | ✔ | `{"online":true,"uuid":…,"fw":…,"ts":…}` — also the LWT (with `online:false`) |
| `<base>/register` | 1 | ✘ | Full capability advertisement, once per connect |
| `<base>/state` | 1 | ✔ | Complete snapshot of all channels |
| `<base>/relay/<ch>/state` | 1 | ✔ | One channel |
| `<base>/diag` | 1 | ✘ | Heartbeat, every 30 s |

### `<base>/relay/<ch>/state`

```json
{
  "channel": 2,
  "name": "Porch",
  "icon": "socket",
  "state": true,
  "source": "physical",
  "rev": 1043,
  "changedAt": 1785432101234,
  "tsSynced": true,
  "group": 1,
  "enabled": true,
  "autoOffInSec": 0
}
```

`source` is one of `physical | app | alexa | schedule | automation | cloud |
restore | timer | factory | unknown`. This is what lets the app say *why*
something changed rather than just *that* it changed.

`rev` is a **monotonic revision**, global across the device and bumped on every
accepted change. It is the conflict-resolution primitive — see below.

`tsSynced: false` means there is no RTC and SNTP has not succeeded yet, so
`changedAt` is an uptime counter, not an epoch. Do not render it as a wall clock
when this is false.

### `<base>/register`

Sent once per connect. Contains uuid, mac, firmware, hardware revision, relay
count, chip info, memory, flash, uptime, reset reason, and a `capabilities`
array the backend uses to decide which UI to offer.

When the device is **unclaimed** it also carries `claimCode` — so the app can
offer "adopt this device" without the user hunting for the enclosure label.
When it **is** claimed it carries `counter` and `sig` (HMAC-SHA256 under the
device key) instead.

---

## Broker → device

| Topic | QoS | Payload |
|---|:--:|---|
| `<base>/relay/<ch>/set` | 1 | see below |
| `<base>/relay/all/set` | 1 | same shape, applies to every channel |
| `<base>/cmd` | 1 | `{"cmd":"…"}` |
| `<base>/config/set` | 1 | partial config patch, same shape as the REST API |

### Relay commands

Bare payloads work:

```
on    off    toggle    1    0    true    false
```

Or JSON, which unlocks the useful options:

```json
{ "action": "on" }
{ "state": true }
{ "action": "on",  "rev": 1044 }        // conflict-safe write
{ "action": "on",  "seconds": 600 }     // on, auto-off after 10 minutes
{ "action": "off", "counter": 8891 }    // replay-protected
```

**`rev` is the important one.** A command carrying a revision is applied **only
if it is newer** than the channel's current revision. This is what stops a
delayed cloud message from undoing a wall-switch press that happened after it
was sent. Omit `rev` for "apply unconditionally".

**`counter`** must strictly increase per device. The firmware persists the
high-water mark across power loss, so replaying a captured message fails.

### `<base>/cmd`

| `cmd` | Effect |
|---|---|
| `snapshot` | Republish the full state and every retained channel topic |
| `diag` | Publish diagnostics immediately |
| `identify` | Emit a heartbeat event (useful for "which box is this?") |
| `reboot` | Flush relay state to NVS, then restart |
| `ota` | Trigger an update check |
| `factory-reset` | Wipe everything — **requires a valid `sig`**, see below |

`factory-reset` must be signed, otherwise one spoofed message could wipe a whole
fleet:

```
sig = HMAC-SHA256(deviceApiKey, "FACTORY\n<baseTopic>\n<counter>\n")
```

---

## Conflict resolution, end to end

The device is the **single source of truth** for relay state. There is exactly
one writer (`RelayManager`), one revision counter, and one event stream.

1. **A physical switch always wins locally.** It is applied immediately and
   reported upward. No network path is involved, so it works with everything
   else down.
2. **A cloud command with `rev` only wins if it is newer.** Otherwise it is
   rejected and the device republishes its current state.
3. **On every reconnect the device republishes everything** (`publishFullState`).
   A reconnecting device re-asserts the truth rather than waiting for the next
   change.
4. The backend reconciles last-write-wins: device timestamp for `physical`
   changes, server timestamp for user-initiated ones.

---

## Broker choice on a free budget

**Render's free tier cannot host a broker.** It exposes HTTP/WS only — no raw
TCP 1883/8883 — and it sleeps after 15 minutes of inactivity. A sleeping broker
means your relays stop responding remotely.

Use **HiveMQ Cloud Serverless (free)**: always on, TLS on 8883, ~100
connections, 10 GB/month. Render then handles only REST/OTA/dashboard, where a
cold start is tolerable because realtime does not depend on it.

Consequence: **`mqttTls` must be `true` for any cloud deployment**, because
HiveMQ Cloud is TLS-only. Plaintext MQTT remains available only for a local
Mosquitto during development.

---

## Per-device ACLs are mandatory

Without them, one leaked device credential can subscribe to `#` and switch every
relay of every customer. Give each device credentials scoped to its own subtree:

```
publish    smarthome/<homeId>/<uuid>/#
subscribe  smarthome/<homeId>/<uuid>/#
```

The backend gets a separate account with broader access. This is the single most
dangerous thing to get wrong in a multi-tenant deployment.

---

## Debugging

```bash
# Everything from every device
mosquitto_sub -h <host> -p 8883 --capath /etc/ssl/certs \
              -u <user> -P <pass> -t 'smarthome/#' -v

# Turn on channel 2
mosquitto_pub -h <host> -p 8883 --capath /etc/ssl/certs \
              -u <user> -P <pass> \
              -t 'smarthome/home_7f3a/sh-a4cf12b9e0d4/relay/2/set' -m 'on'

# Ask for a fresh snapshot
mosquitto_pub ... -t 'smarthome/home_7f3a/sh-a4cf12b9e0d4/cmd' \
              -m '{"cmd":"snapshot"}'
```

Local development with Mosquitto (`infra/docker-compose.yml`) works the same
way on port 1883 with `mqttTls:false`.
