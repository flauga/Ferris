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

## Connection reliability

BLE links drop at the radio/OS layer (Windows radio power-management, Wi‑Fi 2.4 GHz
coexistence, laptop sleep, a body passing through the RF path, a stall longer than the
supervision timeout). Both sides are now hardened: the **firmware** maximises range and
rides out interference, and the **dashboard** recovers from any drop without ending the
session.

### Firmware (`src/main.cpp`, tunables in `include/config.h`)

1. **Full TX power (+9 dBm).** The radio was previously pinned at **−3 dBm** as a
   brownout workaround — ~12 dB down, roughly a **4× reduction in usable range** and
   the single biggest cause of drops across a room. The brownout detector is already
   disabled and BLE starts after a settle delay, so the workaround was removed.
   Advertising, connection and scan power are set independently (`BLE_TX_POWER_*`).
2. **6 s supervision timeout, zero slave latency.** Was 4 s with latency 2. A longer
   timeout means a brief blockage is *ridden out* rather than killing the link; zero
   latency stops the peripheral skipping connection events, which is what makes a
   marginal link fail. Re-requested ~1 s after connect, because Windows commonly
   accepts the initial request and then imposes its own parameters.
3. **Fast advertising (20–40 ms) + self-heal.** The BT-spec "fast connection" range, so
   the dashboard finds the device again almost instantly after a drop. Advertising is
   also re-asserted every 5 s while unconnected, so a stack that silently fell out of
   advertising can never leave the board invisible until a power-cycle.
4. **Non-blocking re-advertise.** The old path did `delay(500)` inside `loop()`, stalling
   encoder sampling and the data push for half a second on every drop. Now timestamp-driven.
5. **Serial can no longer kill the link.** `Serial.print*` **blocks** once the UART TX
   FIFO fills — which is guaranteed when no monitor is attached. A blocked `loop()`
   starves the BLE task → missed connection events → supervision timeout → *the USB
   debug path was killing the wireless link*. All output now goes through a guard that
   **drops** rather than blocks, and `DIAG` defaults **off** (`DIAG_DEFAULT_ON`).
6. **Task watchdog.** `loop()` is subscribed to the Task Watchdog Timer the Arduino core
   already runs (5 s, panic on timeout). If `loop()` ever wedges the chip reboots — back
   advertising in seconds, brake released, instead of sitting dead until power-cycled.
7. **I2C timeout + bus recovery.** Every transaction is bounded (`I2C_TIMEOUT_MS`), so a
   wedged AS5600 can't hang `loop()`; sustained failures re-initialise the bus.
8. **Peer dead-man.** If the link is nominally up but the dashboard has said nothing for
   `BLE_PEER_TIMEOUT_MS` (15 s), the brake is **released** and the link dropped — so a
   half-open link can never leave a rider pulling against a load nobody is controlling.
9. **Link telemetry in `[STATUS]`** — connect/drop counts, the HCI reason code of the
   last drop, uptime and I2C recoveries, so a flaky link is diagnosable from the
   dashboard log without a serial monitor.
10. **Single-owner brake actuation.** `scaleResistance()` is called only from `loop()`;
    BLE callbacks just set a request flag. The `ledcWrite()` + `currentResistance` store
    is not atomic as a pair, so calling it from two tasks could leave the brake engaged
    while the firmware reported 0 % — and the dashboard echoed that 0 into its slider.
11. **Minimal advertising packet.** The device name lives in the scan response, so the
    advertising packet carries only flags + the service UUID. Shorter packets spend less
    time on air, which helps at the edge of range. The dashboard filters on the name
    **or** the service UUID, so discovery can't break if a scan response is missed.

#### Range: what is and isn't available here

TX power is now at the +9 dBm maximum, modem sleep is disabled (the receiver stays hot
between connection events), and the advertising payload is minimal. Beyond that:

- **BLE 5 "Long Range" (LE Coded PHY) is not possible on this board.** The classic ESP32
  is a Bluetooth 4.2 radio (`CONFIG_BT_BLE_42_FEATURES_SUPPORTED`, no BLE 5), so Coded
  PHY and 2M PHY do not exist in hardware. Getting them means an ESP32-C3/S3/C6 *and* a
  central that negotiates Coded PHY — Web Bluetooth on Chrome/Windows does not today.
- **Wi-Fi coexistence is already a non-issue**: this firmware never starts the Wi-Fi
  driver, so BLE has the radio to itself. (An explicit `esp_wifi_stop()` was tried and
  reverted — it links the whole Wi-Fi driver for +74 KB of flash and zero benefit.)
- **What's left is physical**: antenna keep-out (no ground plane or metal under the PCB
  antenna), orientation (antenna vertical, not pointing end-on at the laptop), and
  avoiding a 2.4 GHz Wi-Fi AP on an overlapping channel near the drum.

### Dashboard (`dashboard/cardio_dashboard.html`)

1. **Auto-reconnect (no chooser).** On a drop it re-runs `gatt.connect()` on the
   retained device object with **exponential backoff (0.6 s → 8 s)**, re-discovering the
   service/characteristics and re-subscribing each time. It keeps trying for **5 minutes**
   of continuous downtime before giving up, so stepping out of range doesn't end a session.
2. **Clean teardown.** The app forces `gatt.disconnect()` so the ESP sees a link-layer
   terminate and re-advertises immediately, instead of waiting out its supervision timeout.
3. **Frame watchdog.** The ESP streams at 25 Hz; if no frame arrives for 2.5 s while
   "connected" (Chrome/Windows sometimes fires `gattserverdisconnected` late or never),
   the link is declared dead, torn down, and recovery starts.
4. **Screen Wake Lock.** Held while connected so the laptop/tablet (and its radio) don't
   sleep mid-session; re-acquired when the tab returns to the foreground.
5. **Bounded frame queue.** Capped (~12 s @ 25 Hz) so a backgrounded tab can't balloon it.
6. **Silent reconnect on reload** via `navigator.bluetooth.getDevices()` — no chooser.
7. **Recoverable-drop UX.** A "Reconnecting…" status + a **Reconnect now** button. A drop
   mid-workout **pauses** the workout (freezing the ramp/elapsed clocks) and **resumes** it
   on reconnect; it only ends & saves on a deliberate Disconnect or after giving up.
8. **Keep-alive ping.** A silent `PING` every 3 s (with a full `S` status every ~30 s) so
   the OS BLE stack sees bidirectional traffic — and so the firmware's peer dead-man
   never fires during normal use.
9. **Resistance is re-asserted on every reconnect.** The firmware releases the brake on
   disconnect, so without this the UI would show 45 % while the hardware sat at 0.
10. **Extra recovery triggers** — retry immediately on `visibilitychange` (the laptop has
    usually just woken) and on `online`, and force a full teardown on bfcache restore.

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
| esp → app status | `[STATUS] {"name":…,"r":<pct>,"heap":<bytes>,"fw":"1.1","conn":<n>,"disc":<n>,"dreason":<hci>,"up":<sec>,"i2crec":<n>}` (text) |
| esp → app info | `[INFO] <message>` (text) |
| app → esp commands | `R:<nn>` (set resistance 0–100) · `RESET` (zero encoder) · `S` (status) · `PING` (silent keep-alive) · `STOP` (release brake) |

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
