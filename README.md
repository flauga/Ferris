# Cardio Drum Trainer

ESP32 firmware + a single-file Web Bluetooth dashboard for a rope-pull **cardio drum**: a rope
wound around a spool spins a drum; an AS5600 magnetic encoder reads drum rotation and a
PWM-controlled magnetic brake provides resistance. Reuses the **Ferra** PCB / brake / AS5600
hardware. No WiFi — communication is over BLE (Nordic UART Service).

## What it does

- **Resistance slider (0–100 %)** drives the PWM brake.
- **Two dials** — drum **RPM** and **Power output**.
- **Start / Stop Recording** — during a recorded session it shows **cumulative energy** plus session
  **average velocity, set-force, and power**.
- **Force / Velocity / Power graph** that **freezes** (with the averages) when the session ends.
- **Reset** clears all values + the graph and zeroes the encoder; **Connect** pairs over Bluetooth.
- **CSV export** auto-downloads on Stop.

## Project layout

```
platformio.ini                 PlatformIO build config (esp32dev / arduino)
include/config.h               pins, PWM, BLE UUIDs, physics record-of-truth
src/main.cpp                   firmware: BLE + AS5600 + PWM brake + 25 Hz data push
dashboard/cardio_dashboard.html  the dashboard (open in Chrome/Edge)
```

## Firmware — build & flash

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code extension).

```sh
pio run -e esp32dev            # build
pio run -t upload              # flash via the attached programmer / USB-UART
pio device monitor -b 115200   # watch the serial output
```

If the programmer enumerates on a fixed COM port, set `upload_port` in `platformio.ini`.

On boot you should see `[INFO] BLE advertising as CardioDrum …` and, once a client connects, a
steady ~25 Hz stream of data lines.

## Dashboard

Web Bluetooth requires a **secure context**: open `dashboard/cardio_dashboard.html` **directly**
(`file://`) or serve it from `localhost` in **Chrome or Edge desktop**. Plain `http://` over a LAN
is blocked by the browser.

1. Click **Connect** and pick **CardioDrum** in the pairing dialog.
2. Drag the **resistance slider** — the brake responds and each data line echoes the enforced value.
3. Click **Start Recording** to begin the chart + energy/averages; **Stop & Save CSV** freezes them
   and downloads the CSV; **Reset** clears everything and zeroes the encoder.

## BLE protocol

Data is sent as a compact **16-byte little-endian binary frame** over the notify
characteristic (efficient, no string parsing, no float-precision loss — the dashboard unpacks it
with `DataView`). Status/info are sent as text on the same characteristic; the dashboard tells them
apart by length (data frames are exactly 16 bytes and don't start with `[`). The firmware also
mirrors each frame as an ASCII line on **USB Serial** for debugging on a plain monitor.

| Direction | Format |
|-----------|--------|
| esp → app data (BLE) | 16-byte LE binary: `u32 ms · f32 rpm · i32 pos · f32 resistance` |
| esp → app data (USB) | ASCII mirror `[<ms>ms] RPM:<rpm> POS:<cum_counts> R:<pct>` (Serial only) |
| esp → app status | `[STATUS] {"name":"CardioDrum","r":<pct>,"heap":<bytes>,"fw":"1.0"}` (text) |
| esp → app info | `[INFO] <message>` (text) |
| app → esp commands | `R:<nn>` (set resistance 0–100) · `RESET` (zero encoder) · `S` (status) · `STOP` (release brake) |

UUIDs (reused from Ferra): service `6e400001-…`, RX/write `6e400002-…`, TX/notify `6e400003-…`.

The dashboard queues incoming frames and drains them once per animation frame, with the chart
redrawn on a separate throttled loop — the same render architecture as the IMU balance board.

## Physics & calibration

All force/velocity/power math runs in the **dashboard JS** so calibration lives in one editable
block (top of the page script). Firmware emits only raw signals (RPM, cumulative counts, resistance).

```
R_EFF_M     = 0.144   m     (drum radius 0.125 + rope radius 0.019)
MAX_FORCE   = 7.5 kgf ≈ 73.6 N  (configurable: edit MAX_FORCE_KGF)

velocity = rpm · 2π/60 · R_EFF_M          (m/s)
set_force = (resistance% / 100) · MAX_FORCE_N   (N, commanded brake force)
power     = set_force · velocity          (W)
energy   += power · dt                     (J)
```

The dials default to **RPM_MAX = 300** and **POWER_MAX = 350 W** (editable JS consts).
POWER_MAX ≈ 73.6 N × ~4.5 m/s (300 rpm @ R_EFF) ≈ 333 W ceiling.

> **Where the 7.5 kgf comes from.** The brake force was measured as **59–62 kgf at the 50 mm-dia
> spool** (radius 0.025 m). What the brake actually holds constant is *torque*, not force:
> 62 kgf × 0.025 m ≈ **15.2 Nm** at full PWM — which is what the old "15 Nm" figure referred to.
> That torque now drives a **250 mm-dia drum with a 38 mm rope** (R_EFF = 0.125 + 0.019 = 0.144 m),
> so the ideal rope force would be 15.2 Nm ÷ 0.144 m ≈ 105 N ≈ 10.7 kgf. After bearing / winding /
> transmission losses the **measured max rope force at the drum is ≈ 7.5 kgf ≈ 73.6 N**, and that
> post-loss figure is the single source of truth (`MAX_FORCE_KGF`). *"Set Force"* is the commanded
> brake force, not a reading from a rope sensor (there is none).

## Safety

The brake is **released (duty 0) at boot and on BLE disconnect**; resistance only rises on an
explicit `R:<nn>` command, and the value is clamped to 0–100.
