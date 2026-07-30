# Architecture

## The one requirement that shapes everything

> A socket must be controllable from **the wall switch**, **the app** and
> **Alexa** — and the ON/OFF state must be correct and visible everywhere, no
> matter which one changed it.

Three independent actors mutating shared state is a distributed-systems problem
wearing a hardware costume. The answer is the usual one: **exactly one writer,
one monotonic revision, one event stream.**

```
     wall switch          app / tablet          Echo Dot
          │                    │                    │
          │ GPIO ISR           │ REST / WebSocket   │ Hue emulation
          │ <2 ms              │                    │
          ▼                    ▼                    ▼
     ┌──────────────────────────────────────────────────┐
     │            RelayManager  (the only writer)       │
     │   • command queue, applied in order              │
     │   • global monotonic `rev`, stamped per channel  │
     │   • rate limit, gang stagger, auto-off timers    │
     └───────────────────────┬──────────────────────────┘
                             │ RelayEvent {ch, state, source, rev, ts}
                             ▼
                        ┌─────────┐
                        │EventBus │  synchronous, handlers must not block
                        └────┬────┘
        ┌──────────┬─────────┼──────────┬────────────┐
        ▼          ▼         ▼          ▼            ▼
      GPIO       NVS      MQTT       WebSocket   Automation
    (already)  (2 s deb.) (retained)  (LAN)       engine
```

Every change carries **who caused it** (`source`) and **when** (`rev`, `ts`), so
the app can say *"Porch turned on — wall switch, 2 seconds ago"* rather than
just showing a green dot.

### Conflict rules

1. **The physical switch always wins locally.** No network is in its path, so it
   works with everything else down. It applies immediately, then reports upward.
2. **A cloud command carrying `rev` wins only if it is newer.** This stops a
   delayed message from undoing a wall-switch press that happened after it was
   sent.
3. **On reconnect the device republishes everything.** It re-asserts the truth
   instead of waiting for the next change.

### The latching-switch desync fix

An Indian modular rocker stays where you left it. After the app turns a socket
off, the switch is still physically in the "on" position. Toggling to *match the
switch position* would then do nothing on the next flip.

So any accepted position change toggles **relative to the relay's current
state**, not to the switch's. Flip it, and the state changes. Always.

---

## Boot order (not arbitrary)

```
1. Logger              so everything after it can explain itself
2. NVS + Config        the relay restore policy lives here
3. RELAYS              before Wi-Fi, before the web server, before anything slow
4. Switches            wall switches work even if the rest fails to start
5. Everything else     Wi-Fi, HTTP, MQTT, scheduler, automation, diagnostics
```

Step 3 is the point. Every millisecond spent elsewhere first is a millisecond
the coils spend in an undefined state. (The external pull-ups are what actually
hold them off during reset — `WIRING.md` §1.1. Boot order keeps the window short
afterwards.)

---

## Tasks

| Task | Core | Prio | Stack | Watchdog | Role |
|---|:--:|:--:|--:|:--:|---|
| `switch` | 1 | 5 | 3072 | ✔ | ISR-woken, debounce, gestures |
| `relay` | 1 | 4 | 3072 | ✔ | Owns every GPIO write |
| `wifi` | 0 | 3 | 4096 | ✔ | Scan, connect, backoff, SoftAP |
| `httpsvc` | 0 | 2 | 6144 | ✔ | Captive DNS, WebSocket broadcast |
| `sched` | 1 | 2 | 4096 | ✔ | 1 s tick |
| `auto` | 1 | 2 | 4096 | ✔ | Rule actions, delays |
| `diag` | 0 | 1 | 4096 | ✔ | 30 s heartbeat, heap/RSSI watch |
| `ota` | 0 | 1 | 8192 | ✘ | Created on demand, deleted after |
| *(esp_mqtt)* | — | — | 6144 | — | Owned by the IDF client |

**Why `ota` is not watchdog-subscribed:** a flash erase can outlast the watchdog
period, and a reset mid-write is precisely how a device gets bricked.

**Why the watchdog is 15 s, not the 5 s default:** a TLS handshake on a weak
link, plus a blocking Wi-Fi scan, can legitimately hold a task for several
seconds. A watchdog that fires on healthy work is worse than none.

The switch task sits *above* the relay task so a press is queued the instant it
is seen. Latency budget, measured end to end: **< 20 ms**, and typically ~2 ms
because nothing on that path touches the network.

---

## The EventBus contract

Dispatch is **synchronous** — `publish()` runs every handler on the calling
task. That keeps ordering strict and avoids a second queue hop on the
latency-critical relay path.

The price is a hard rule: **a handler must never block.** It enqueues onto its
own service queue and returns. A handler doing network I/O inline would stall
the relay task and break the switch-latency guarantee. Both MQTT and the
WebSocket bridge follow this — they enqueue and let their own task do the work.

---

## Offline-first, concretely

Not a slogan. With the internet down:

| Still works | Why |
|---|---|
| Wall switches | Never touched the network |
| The app, over LAN | On-device REST + WebSocket, found via mDNS |
| Alexa voice | Hue emulation is a local network call |
| Schedules | Run on-device from NVS |
| Automations | Same |
| State restore after a power cut | Persisted in NVS |

What stops: remote access, cloud sync, OTA, and the AI features. The device
degrades to "a very good local smart switch", which is the correct failure mode.

The honest gap: **there is no RTC.** After a power cut with no internet the
device genuinely does not know the time. The scheduler refuses to fire rather
than guessing, and every timestamp is tagged `tsSynced` so nothing downstream
mistakes an uptime counter for a wall clock. Add a DS3231 (~₹100) if that
matters.

---

## Storage

| Namespace | Contents | Write frequency |
|---|---|---|
| `sh-cfg` | Device + channel config, as JSON | On change |
| `sh-rly` | Relay states + revision, CRC-checked blob | Debounced 2 s |
| `sh-wifi` | Up to 5 networks, CRC-checked | On change |
| `sh-sec` | API key, claim code, replay counters | Rare / blocked |
| `sh-sch` | Schedules | On change |
| `sh-aut` | Automation rules | On change |
| `sh-sys` | Boot counter | Once per boot |

NVS endures ~100k cycles. A naive "persist on every toggle" would reach that on
a heavily used channel, so relay state is **debounced 2 s and written only if
changed**. The replay counter uses a different trick: reserve a block of 100 in
flash, hand them out from RAM, and only touch flash when the block runs out. A
power loss skips forward, which is fine — the requirement is *strictly
increasing*, not *gapless*.

Every persisted blob carries a CRC32. A half-written blob after a brownout is
detected and rejected rather than silently applied to the relays.

---

## Flash layout (4 MB)

```
nvs       20 KB    otadata    8 KB
app0     1.6 MB    app1     1.6 MB     ← dual slots, real rollback
littlefs 512 KB    coredump  64 KB
```

The web UI is embedded in the **app image**, not LittleFS — a filesystem image
is a separate upload step people forget, and a corrupt filesystem would leave a
bricked-looking device with no way to reconfigure it.

---

## Security, and its limits

| In v1 | Deferred to Phase 4 | Why deferred |
|---|---|---|
| Per-device API key (revocable) | mTLS client certs | Needs backend PKI |
| HMAC-SHA256 on cloud payloads | — | — |
| Persisted replay counter | — | — |
| TLS to the cloud (mandatory) | TLS on the LAN too | Heap budget |
| SHA-256 verified OTA + rollback | Signed images (Ed25519) | Needs a signing key |
| WPA2 SoftAP, per-device PIN | BLE provisioning | — |
| Claim-code adoption flow | — | — |
| NVS-stored secrets | eFuse flash encryption + secure boot | **Irreversible per chip**, and it makes development reflashing painful |

Two limits stated plainly:

- **NVS is not encrypted.** Anyone with the board and a USB cable can read the
  Wi-Fi password and the API key out of flash. Treat the key as *revocable*, not
  as *secret from someone holding the hardware*.
- **The LAN API is plain HTTP.** Anyone already on your network can read it.

Devices hold **API keys, not JWTs** — a JWT expires, and a device offline for a
month cannot refresh one, so it would lock itself out permanently. JWTs are for
user sessions.

---

## Heap budget

320 KB total. With TLS active, roughly:

| Consumer | Approx. |
|---|---|
| Wi-Fi + LWIP | 50 KB |
| One mbedTLS session (`MBEDTLS_SSL_MAX_CONTENT_LEN=4096`) | 25 KB |
| esp_mqtt client + outbox | 15 KB |
| AsyncWebServer + AsyncTCP | 20 KB |
| Task stacks (9 tasks) | 40 KB |
| Application | 15 KB |
| **Free at idle** | **~120 KB** |

Two deliberate choices keep this workable: the reduced mbedTLS record buffer
(saves ~28 KB), and **one TLS context alive at a time** — MQTT's TLS is torn
down while OTA's runs. Diagnostics warns below 40 KB and flags critical below
25 KB, because a TLS handshake starts failing before the heap actually runs
out, and "MQTT randomly stopped working" is a miserable thing to debug after the
fact.

---

## Future expansion

The module boundary is `RelayChannel`. A dimmer, RGB light, fan controller or
curtain motor is a channel with a different actuator and a wider state type;
`ChannelState` grows a `level` field and `RelayManager` keeps its role as the
single writer.

One hard limit worth knowing now: **classic ESP32 cannot do Zigbee or Thread.**
There is no 802.15.4 radio — that needs an ESP32-C6 or -H2. This is silicon, not
software. Matter over Wi-Fi is possible on this chip but needs ~1.2 MB and a
different partition scheme.
