# Cardio Drum — Android app

A native Android (Kotlin + Jetpack Compose) controller for the Cardio Drum Trainer.
It is the mobile counterpart to `dashboard/cardio_dashboard.html`: it connects to the
`CardioDrum` ESP32 over Bluetooth LE, **varies the brake resistance**, **monitors drum
RPM**, computes **power output**, and records everything as **sessions** you can start and
stop. When a session ends it shows the **cumulative work output (joules)** — an objective
number for comparing one session against another — and saves it to an on-device history.

## Features

- **Connect** to the `CardioDrum` over BLE (Nordic UART Service), with auto-reconnect,
  a keep-alive, and a frame watchdog — the same reliability model as the web dashboard.
- **Resistance slider (0–100 %)** drives the PWM brake in real time.
- **Live dials** — drum **Speed (RPM)** and **Power (W)** — plus velocity and set-force tiles.
- **Sessions** — Start/Stop. While recording it shows live **cumulative work**, elapsed
  time, average power, distance and peak power.
- **End-of-session summary** — the total **work output** in J/kJ, compared against your
  previous best (`+x %` / new best), plus the session's averages and peaks.
- **History** — every saved session listed with its work output and a relative-work bar,
  so sessions are directly comparable. Your best session is flagged.

All force/velocity/power/energy math lives in one calibration object
(`data/Physics.kt`) and mirrors the firmware's record-of-truth and the dashboard JS.

## Build & run

Requires **Android Studio** (Ladybug / 2024.2 or newer) with an Android SDK, or a
command-line SDK. The project targets **compileSdk 35** and **minSdk 26 (Android 8.0)**.

### Android Studio
1. `File ▸ Open…` and select this **`android/`** directory.
2. Let Gradle sync (it downloads AGP 8.7, Kotlin 2.0, Compose and Room).
3. Plug in a phone with **USB debugging** on (or an emulator — note BLE needs a real
   device) and press **Run**.

### Command line
```sh
cd android
# Point at your SDK (or set the ANDROID_HOME / ANDROID_SDK_ROOT env var):
echo "sdk.dir=$HOME/Android/Sdk" > local.properties
./gradlew :app:assembleDebug          # build the APK
./gradlew :app:installDebug           # build + install on the attached device
```
The APK lands in `app/build/outputs/apk/debug/`.

> A BLE scan/connect requires a **physical device** — emulators have no Bluetooth radio.

## Using it

1. Power on the drum (`[INFO] BLE advertising as CardioDrum …` on its serial monitor).
2. Open the app and tap **Connect to CardioDrum**. Grant the Bluetooth permission prompt
   (Android 12+: *Nearby devices*; Android ≤11: *Location*, needed only for the scan).
3. Drag the **Resistance** slider — the brake responds and the enforced value echoes back.
4. Tap **Start Session** to begin recording. Pull the rope; watch RPM, power and the
   cumulative work climb.
5. Tap **Stop Session** — the summary shows your total **work output** and how it compares
   to your best. It's saved to **History** (top-right clock icon) for later comparison.

## How it talks to the firmware

Nordic UART Service, identical UUIDs and frame layout to the firmware and dashboard
(`include/config.h`, `src/main.cpp`):

| Direction | Format |
|-----------|--------|
| esp → app data | 16-byte little-endian: `u32 ms · f32 rpm · i32 pos · f32 resistance` |
| esp → app text | `[STATUS] {…}` / `[INFO] …` (any notification that isn't 16 bytes) |
| app → esp | `R:<0–100>` · `RESET` · `S` · `PING` (keep-alive) · `STOP` (release brake) |

- Service `6e400001-…`, RX/write `6e400002-…`, TX/notify `6e400003-…`, device name `CardioDrum`.
- A silent `PING` every 3 s (and a `S` every ~30 s) keeps the link warm so the firmware's
  30 s peer dead-man never releases the brake mid-session.
- The firmware **releases the brake on every disconnect**, so the app re-asserts the
  commanded resistance on each (re)connect.

## Physics / calibration

Mirrors `include/config.h`. To recalibrate, edit these **and** the firmware:

```
R_EFF_M   = 0.144 m           (drum radius 0.125 + rope radius 0.019)
MAX_FORCE = 7.5 kgf ≈ 73.6 N  (measured max rope force at full PWM)

velocity = |rpm|·2π/60·R_EFF        (m/s)
set_force = (resistance%/100)·MAX_FORCE_N   (N, commanded brake force)
power    = set_force · velocity     (W)
work     = ∫ power dt               (J)   ← the session's objective number
distance = ∫ velocity dt            (m)
```

## Project layout

```
app/src/main/java/com/aroleap/cardiodrum/
  MainActivity.kt            Compose host, navigation, BLE runtime-permission flow
  ble/BleConstants.kt        UUIDs, commands, timeouts (mirrors firmware)
  ble/Frame.kt               16-byte LE frame decode
  ble/ConnectionState.kt     link state surfaced to the UI
  ble/CardioBleManager.kt    scan → connect → notify → write, queue, keep-alive, reconnect
  data/Physics.kt            force/velocity/power/energy — the calibration block
  data/SessionEntity.kt      a saved session (workJ = cumulative work output)
  data/SessionDao.kt         Room DAO
  data/AppDatabase.kt        Room database
  data/SessionRepository.kt  persistence wrapper
  ui/CardioViewModel.kt      BLE → physics → live state, session integration + save
  ui/LiveState.kt            live + session-summary models
  ui/DashboardScreen.kt      gauges, resistance, live stats, session controls
  ui/SessionSummaryDialog.kt end-of-session work-output comparison
  ui/HistoryScreen.kt        saved sessions, compared by work output
  ui/Widgets.kt              arc gauge + stat tiles
  ui/theme/Theme.kt          dark HUD palette (matches the dashboard)
```
