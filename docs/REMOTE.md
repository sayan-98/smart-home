# Control from Anywhere

Forgot a light and you're at the office. This is how you turn it off.

---

## How it works

```
   ESP32  ──outbound TLS──▶  broker (cloud)  ◀──outbound WSS──  your phone
  (at home)                   always on                        (anywhere)
```

Both ends dial **out**. Neither needs a public IP, a port forward, a static
address, or a domain — which matters, because a phone hotspot and most Indian
broadband put you behind carrier-grade NAT, where inbound connections are simply
impossible.

Two firmware properties make this simpler than it looks:

- **State topics are retained.** The instant the app connects, the broker
  replays the current state of all eight sockets. The screen is correct within a
  second of opening it — no polling, no loading spinner.
- **Devices are discovered from those same retained messages.** There is no
  registry to configure. Subscribing to the wildcard *is* the registry.

---

## The one prerequisite

> **Your home needs internet that stays on while you're out.**

Obvious once said, but worth saying: if the ESP32's only internet is the phone
in your pocket, then when you leave, the device has no network. Nothing can
reach it. No backend fixes that.

Any of these work — the firmware doesn't care which:

- a broadband router (best)
- an old phone left plugged in as a permanent hotspot
- a JioFi / 4G dongle

It must be **2.4 GHz** — the ESP32 has no 5 GHz radio.

---

## Setup, once

### 1. Create a free broker

Sign up at **HiveMQ Cloud** and create a *Serverless* cluster. Free, no card.
Then add an *Access Credential* (a username and password) with publish and
subscribe rights.

Note three things from the cluster page:

| | Example |
|---|---|
| Hostname | `abc123def456.s1.eu.hivemq.cloud` |
| **WebSocket** port | `8884` |
| MQTT (TLS) port | `8883` |

**The two ports are different and it matters.** The app is a web app and can
only speak WebSocket, so it uses **8884**. The ESP32 speaks native MQTT and uses
**8883**. Putting 8883 in the app is the most common mistake here.

*(EMQX Serverless works identically if you prefer it — just a different
hostname.)*

### 2. Point the device at the broker

On the same Wi-Fi as the device:

```bash
curl -X POST http://smarthome-XXXX.local/api/config \
  -H 'Content-Type: application/json' \
  -d '{"device":{
        "cloudEnabled": true,
        "mqttHost": "abc123def456.s1.eu.hivemq.cloud",
        "mqttPort": 8883,
        "mqttTls": true,
        "mqttUser": "your-credential-name",
        "mqttPass": "your-credential-password",
        "homeId": "home"
      }}'
```

Then reboot it:

```bash
curl -X POST http://smarthome-XXXX.local/api/reboot
```

Watch the serial monitor to confirm:

```
[mqtt] connecting to mqtts://abc123def456.s1.eu.hivemq.cloud:8883 (tls=yes)
[mqtt] connected
[mqtt] subscribed under smarthome/home/sh-841fe867ab10
[mqtt] registration published
```

### 3. Point the app at the broker

Open the app → **Anywhere** tab → enter the hostname, port **8884**, and the
same username and password. Tap **Connect**.

The badge changes to **"Remote — via broker"** and your sockets appear.

---

## Using it on an iPhone

The app is Android. An iOS build needs a Mac and a paid Apple developer account.

The free route: this is a web app underneath, so publish it and open it in
Safari.

```bash
cd app
npm run build          # output lands in app/dist
```

Host `dist/` anywhere static and free — **GitHub Pages** is the obvious choice
since the repo already exists. Then on the iPhone, open the URL in Safari and
tap **Share → Add to Home Screen**. It gets an icon and runs full-screen, and
the *Anywhere* tab works exactly as it does on Android.

One limitation: a page served over HTTPS cannot make plain-HTTP requests, so the
**This Wi-Fi** tab won't work from the hosted version. Use *Anywhere* on the
iPhone — it works at home and away.

---

## What this does and does not give you

| Works | Doesn't |
|---|---|
| Switch any socket from anywhere | Voice from outside the house |
| Live state — a wall-switch press shows up on your phone in the office | Family members with separate logins |
| Alexa at home (local, unaffected) | History of who switched what |
| Schedules and automations (on-device) | The AI command bar |

The right-hand column needs the Render + Supabase backend, which is already
written and sitting in `backend/`. Deploying it is a deployment job, not a
development one — see the root README.

---

## Cost

HiveMQ Cloud Serverless free tier: roughly **100 concurrent connections** and
**10 GB/month**.

Your actual usage with one board:

- **3 connections** — the ESP32, the tablet, the phone
- **~35 MB/month** — a 30-second heartbeat plus state changes

About 0.3% of the allowance. Pricing does change, so check the plan page when
you sign up.

---

## When something is wrong

| Symptom | Cause |
|---|---|
| App: "The broker rejected those credentials" | Wrong username/password, or the credential lacks publish/subscribe rights |
| App connects, but no devices appear | The ESP32 hasn't reached the broker. Check its serial log for `[mqtt] connected` |
| Devices appear but show stale state | The device is offline. Its `status` topic is retained, so the last state lingers — check the online badge |
| Nothing after a power cut at home | The device is fine, its internet isn't. This is the prerequisite above |
| Works at home, not from mobile data | You used port 8883 in the app instead of **8884** |
| Serial shows `MQTT_CONNECTION_REFUSED` | Credentials, or `mqttTls` set to false against a TLS-only broker |

The device keeps a log you can read without a cable:

```bash
curl http://smarthome-XXXX.local/api/diag | jq .log
```

---

## Security, stated plainly

- Traffic to the broker is **TLS in both directions**, verified against the
  bundled Mozilla root store.
- Commands carry a **monotonic revision**, so a command sent over a slow mobile
  link cannot undo a wall-switch press that happened while it was in flight.
- The broker credentials are stored on the device in **unencrypted NVS**, and in
  the app in Capacitor Preferences. Anyone with physical access to either can
  read them. Rotate the credential in the broker dashboard if a device is lost —
  that is the revocation path.
- **Give the device its own broker credential**, separate from the app's, and
  scope its ACL to `smarthome/home/<its-uuid>/#`. Without that, one leaked
  device credential can subscribe to `#` and control everything.
