# Build Environment

Every version pin here was arrived at by hitting the failure it prevents. They
are recorded so the next person does not have to rediscover them.

---

## Firmware

| Requirement | Version | Why this one |
|---|---|---|
| Python | 3.12 (**official CPython**) | The PlatformIO installer rejects Anaconda's Python outright |
| PlatformIO Core | 6.1.19, in its **own venv** | Installed into Anaconda instead, the platform's post-install hook fails with `FileNotFoundError [WinError 2]` |
| Platform | **pioarduino** 54.03.21 | See below |
| `click` (in PlatformIO's venv) | **< 8.2** | 8.2 breaks the bootloader packaging step: `ParamType.get_metavar() missing 1 required positional argument: 'ctx'` |
| MinGW-w64 (Windows) | any | Only for `pio test -e native`; Windows has no host C++ compiler by default |

### Why pioarduino and not `platformio/espressif32`

The official platform is frozen at **arduino-esp32 2.x / IDF 4.4**, where:

- `esp_mqtt_client_config_t` still uses the flat pre-IDF-5 layout, so
  `mc.broker.address.uri` and friends do not compile;
- the certificate bundle symbol is the Arduino shim
  `arduino_esp_crt_bundle_attach`, not `esp_crt_bundle_attach`;
- AsyncTCP 3.x (which ESPAsyncWebServer 3.7+ requires) is unsupported.

This firmware targets **arduino-esp32 3.2.1 / IDF 5.4** throughout, which
pioarduino provides.

### Setup

```bash
# Official Python, then PlatformIO into its own venv
winget install Python.Python.3.12
curl -o get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
"$LOCALAPPDATA/Programs/Python/Python312/python.exe" get-platformio.py

# The click pin
"$USERPROFILE/.platformio/penv/Scripts/python.exe" -m pip install "click<8.2"

# Native tests need a host compiler on Windows
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

### Disk space

The toolchain is about **4 GB**. If your system drive is tight, move the package
store off it — this is not optional, a full disk fails mid-extract with
`[WinError 112]` and leaves a corrupt toolchain:

```powershell
[Environment]::SetEnvironmentVariable('PLATFORMIO_CORE_DIR', 'F:\platformio-core', 'User')
```

### Build

```bash
cd firmware
pio run -e esp32dev                              # build
pio run -e esp32dev -t upload --upload-port COM8 # flash
pio device monitor -p COM8 -b 115200             # watch
pio test -e native                               # 13 logic tests
```

Expect roughly: **RAM 20.4 %, Flash 84.6 %** of the 1.875 MB app slot.

---

## Backend

Node 20+.

```bash
cd backend
cp .env.example .env
npm install
npx prisma generate
npx prisma db push      # needs DIRECT_URL, not the pooled DATABASE_URL
npm run dev
```

---

## App

| Requirement | Version | Why |
|---|---|---|
| Node | 20+ | |
| **JDK** | **21** | Capacitor 7's Android module compiles at source level 21. JDK 17 fails with `invalid source release: 21` |
| Android SDK | platform-tools + build-tools | Android Studio itself is optional |

```bash
winget install EclipseAdoptium.Temurin.21.JDK
# then set JAVA_HOME to it
```

```bash
cd app
npm install
npm run build
npx cap add android          # once
npm run android:sync
cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### React 19

React 19 removed the **global `JSX` namespace**. Components annotated
`: JSX.Element` need an explicit import:

```ts
import type { JSX } from 'react';
```

### Redmi / HyperOS

Developer Options needs **both**:

- *USB debugging*
- *Install via USB* — this one requires being signed into a Mi account, and
  sometimes a SIM in the device. Budget 15 minutes the first time.

Confirm the tablet is visible before building:

```bash
adb devices -l     # should list the device as "device", not "unauthorized"
```

---

## Things that will waste an hour if you do not know them

| Symptom | Cause |
|---|---|
| `invalid source release: 21` | JDK 17; Capacitor 7 needs 21 |
| `Cannot find namespace 'JSX'` | React 19; import the type |
| `ParamType.get_metavar() missing 1 required positional argument` | `click` 8.2 in PlatformIO's venv |
| `[WinError 112] There is not enough space on the disk` | Toolchain needs ~4 GB; set `PLATFORMIO_CORE_DIR` |
| `FileNotFoundError` during platform install | PlatformIO installed into Anaconda instead of its own venv |
| `'g++' is not recognized` | No host compiler for the native tests |
| `esp_mqtt_client_config_t has no member named 'broker'` | Wrong platform — you are on arduino-esp32 2.x |
| Program size exceeds maximum | Using the default single-app partition table instead of `partitions_4mb_ota.csv` |
