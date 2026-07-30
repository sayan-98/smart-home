# OTA Update Guide

## What makes this safe

Two 1.875 MB app slots. An update writes the inactive one, marks it for boot and
reboots. The part that matters is what happens *next*:

> The new image is marked **valid** only after it boots, associates to Wi-Fi and
> — when the cloud is enabled — reaches the MQTT broker, and holds that for 45
> seconds. If it panics, or boots but cannot get online, the bootloader falls
> back to the previous slot on the next reset.

Marking an image valid just because `setup()` ran would defeat the whole point.
The classic bad OTA is one that boots perfectly and then cannot reach the
network — precisely the case that must roll back, and the one you cannot fix
remotely because the fix would have to arrive over the channel that broke.

Integrity is checked **before** any boot flag is touched: the manifest carries a
SHA-256, verified while streaming. A truncated or corrupted download never
becomes bootable.

---

## Publishing a release

### 1. Build with a bumped version

```ini
# firmware/platformio.ini
-D SH_FW_VERSION=\"1.1.0\"
```

```bash
cd firmware
pio run -e esp32dev
```

The image is at `.pio/build/esp32dev/firmware.bin`.

### 2. Hash it

```bash
# Windows PowerShell
(Get-FileHash .pio\build\esp32dev\firmware.bin -Algorithm SHA256).Hash.ToLower()

# bash
sha256sum .pio/build/esp32dev/firmware.bin
```

### 3. Upload

Supabase Storage (free tier) works well: create a **public** bucket `firmware`,
upload as `firmware-1.1.0.bin`, and use the public URL. Signed URLs also work —
the firmware follows redirects, which is what Supabase and Render both issue.

### 4. Register the release

```bash
curl -X POST https://your-app.onrender.com/api/ota/releases \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{
        "version": "1.1.0",
        "hardware": "devkitv1-8ch",
        "url": "https://xxxx.supabase.co/storage/v1/object/public/firmware/firmware-1.1.0.bin",
        "sha256": "…64 hex chars…",
        "notes": "Fixes latching switch desync on channel 7",
        "rolloutPercent": 0
      }'
```

**`rolloutPercent` defaults to 0** — a new release reaches nobody until you
deliberately widen it. Opting in beats opting out when the mistake is
unfixable.

### 5. Stage the rollout

```bash
# One device first - yours.
curl -X PATCH .../api/ota/releases/<id> -d '{"rolloutPercent":5}'

# Watch it
curl .../api/ota/rollout/<id>
# → { "total": 40, "updated": 2, "inWave": 2, ... }

# Widen once you are satisfied
curl -X PATCH .../api/ota/releases/<id> -d '{"rolloutPercent":25}'
curl -X PATCH .../api/ota/releases/<id> -d '{"rolloutPercent":100}'
```

Rollout membership is a stable hash of the device UUID, so a device that is in
a 5 % wave stays in it — it does not flap in and out between polls.

**Kill switch:** set `rolloutPercent` back to `0`. No further devices pick it
up. Devices already running it are unaffected; they roll back on their own only
if the image never proved it could get online.

---

## Triggering an update

Devices do not poll on a timer by default — that would burn free-tier requests
for nothing. Trigger explicitly:

```bash
# via MQTT
mosquitto_pub -t 'smarthome/<homeId>/<uuid>/cmd' -m '{"cmd":"ota"}'

# via the backend
curl -X POST .../api/devices/<id>/command -d '{"cmd":"ota"}'

# directly, on the LAN
curl -X POST http://smarthome-XXXX.local/api/ota/check -d '{}'

# from a specific URL, bypassing the manifest
curl -X POST http://smarthome-XXXX.local/api/ota/check \
     -d '{"url":"http://192.168.1.5:8000/firmware.bin","sha256":"…"}'
```

Watch progress:

```bash
curl http://smarthome-XXXX.local/api/ota/status
# { "state":"downloading", "progress":62, "partition":"app0", "version":"1.0.0" }
```

The status LED strobes continuously during an update.

---

## Testing rollback (do this once, deliberately)

You want to know this works *before* you need it.

1. Build `1.1.0-broken` with the Wi-Fi credentials deliberately wrong, or an
   early `abort()` after boot.
2. Register it with `rolloutPercent: 100` on a test device only.
3. Trigger the update.
4. The device installs it, reboots, fails to get online, and **does not** mark
   the image valid.
5. Power-cycle it. The bootloader falls back to the previous slot.
6. Confirm: `curl http://smarthome-XXXX.local/api/ota/status` reports the old
   version and the other partition.

The serial log tells you where you are:

```
[ota] running an UNCONFIRMED image on 'app1' - it rolls back on the next
      reset unless it proves it can get online
...
[ota] image confirmed - rollback cancelled
```

---

## Certificate expiry — the trap this avoids

A single pinned root CA baked into firmware **expires**. Let's Encrypt roots do
rotate. When yours expires, every device in the field permanently loses the
ability to update — and the fix would have to arrive over the very channel that
just broke. There is no remote recovery; it means physically reflashing every
unit.

This firmware uses the **bundled Mozilla root store**
(`esp_crt_bundle_attach`) instead, for both MQTT and OTA. The bundle travels
with the firmware, so each update refreshes it.

The remaining risk is a device that goes offline for years and misses every
update. For a long-lived fleet, plan a periodic maintenance release whose only
job is to refresh the trust store.

---

## Failure modes

| Symptom | Cause | Fix |
|---|---|---|
| `no OTA partition available` | Flashed with a single-app partition table | Reflash over USB with `partitions_4mb_ota.csv` |
| `image size N does not fit` | Firmware outgrew the 1.875 MB slot | Reduce `SH_LOG_LEVEL`, or shrink the app |
| `SHA-256 mismatch` | Wrong hash, or a corrupted upload | Re-hash the exact file you uploaded |
| `HTTP 404` | Bucket is private, or the URL is wrong | Make the bucket public, or use a signed URL |
| `connect failed: ESP_ERR_ESP_TLS...` | Server certificate not in the Mozilla bundle | Use a normal public CA, not self-signed |
| `download stalled` | Weak Wi-Fi | Check RSSI in `/api/diag`; below −78 dBm is trouble |
| Device reboots mid-update | Brownout — the 5 V supply | `docs/WIRING.md` §1.2. Fix this before updating in the field |
| Update succeeds, device never comes back | The new image cannot get online | It rolls back on the next power cycle, by design |

---

## Development: serving from your PC

```bash
cd firmware/.pio/build/esp32dev
python -m http.server 8000
```

```bash
curl -X POST http://smarthome-XXXX.local/api/ota/check \
     -H 'Content-Type: application/json' \
     -d "{\"url\":\"http://192.168.1.5:8000/firmware.bin\",\"sha256\":\"$(sha256sum firmware.bin | cut -d' ' -f1)\"}"
```

Plain HTTP is accepted for exactly this case and logs a warning. It is fine on
your own LAN and nowhere else.
