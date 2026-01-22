# Smart Irrigation System (ESP32 + Firebase + Web App)

A smart irrigation project built with **ESP32**, a **capacitive soil moisture sensor (analog)**, **HC-SR04 ultrasonic sensor**, **relay + pump**, **SH1106 I2C OLED**, and **Firebase Realtime Database**.  
The system supports **two-way synchronization** between the ESP32 firmware and a **Web App UI** via Firebase, and provides local control using **two push buttons** and **two KY-040 rotary encoders**.

---

## Key Features

- **Soil moisture monitoring** (`Soid` in %) using a capacitive soil sensor (AO → ADC).
- **Remaining water estimation** (`Volume` in ml) using HC-SR04 distance measurement and a container volume model.
- **SH1106 OLED dashboard**:
  - Soil moisture (%)
  - Water volume (ml)
  - Status line with current configuration: `Th:<threshold>  M:<mode>  T:<pump>s`
- **Low-water protection**
  - If `Volume < 200 ml`, the pump is forcibly disabled.
  - A warning LED blinks **500 ms ON / 500 ms OFF**.
- **Three operating modes** (controlled via Firebase/Web App and local Mode button):
  1. **MANUAL (Mode=0)**: Pump toggles ON/OFF directly (no threshold/timer).
  2. **AUTO (Mode=1)**: If `Soid < Threshold`, run pump for `PumpSeconds`, then stop (with cooldown).
  3. **SCHEDULE (Mode=2)**: Run pump at a scheduled date/time for `PumpSeconds`.
- **Two KY-040 rotary encoders** (local tuning with Firebase sync):
  - Encoder #1: **Threshold (%)**
  - Encoder #2: **PumpSeconds (s)**
  - Encoder switch toggles step size **1 ↔ 5**
- **Serial monitoring** output every **5 seconds** (key values printed with `----` separator).

---

## Hardware Overview

- ESP32 development board (ESP32-WROOM / NodeMCU / LuaNode32 compatible)
- Relay module + pump (e.g., RS385)
- Capacitive soil moisture sensor module (analog output)
- HC-SR04 ultrasonic sensor module
- SH1106 OLED module (I2C)
- 2× push buttons
- 2× KY-040 rotary encoders
- 1× warning LED (low-water indicator)
- 1x pump indicator LED
---

## Pin Mapping (Firmware-Accurate)

### Core I/O

| Function / Module | Signal | ESP32 GPIO |
|---|---|---:|
| Relay (Pump) | IN | **27** |
| Pump indicator LED | LED | **16** |
| Button #1 (Pump toggle) | INPUT_PULLUP | **32** |
| Button #2 (Mode cycle) | INPUT_PULLUP | **33** |
| Low-water warning LED | LED | **17** |
| Soil moisture sensor | AO (ADC) | **34** |
| OLED SH1106 (I2C) | SDA | **21** |
| OLED SH1106 (I2C) | SCL | **22** |

### HC-SR04 Ultrasonic

| Module | Signal | ESP32 GPIO |
|---|---|---:|
| HC-SR04 | TRIG | **18** |
| HC-SR04 | ECHO | **35** *(input-only, recommended)* |

### Rotary Encoders (2× KY-040)

**Encoder #1 — Threshold**
| KY-040 | ESP32 GPIO |
|---|---:|
| CLK | **25** |
| DT  | **26** |
| SW  | **14** |

**Encoder #2 — PumpSeconds**
| KY-040 | ESP32 GPIO |
|---|---:|
| CLK | **19** |
| DT  | **23** |
| SW  | **13** |

---

## Electrical Interface Requirements (Logic Levels)

ESP32 GPIO operates at **3.3V logic**. If any connected module produces **5V logic outputs**, use appropriate **level shifting or resistor dividers** before connecting signals to ESP32 input pins.

Common cases that typically require attention:
- **HC-SR04 ECHO** is commonly **5V** → level-shift before **GPIO35**.
- **KY-040** modules powered at 5V commonly output **5V** on **CLK/DT/SW** → level-shift these signals.
- **I2C OLED** modules may pull SDA/SCL up to the module supply; verify pull-up voltage and apply I2C level shifting if needed.

---

## Firebase Realtime Database Schema

Base path: **`/Var`**

| Key | Type | Description |
|---|---|---|
| `Soid` | int | Soil moisture (%) reported by ESP32 |
| `Volume` | float | Remaining water (ml) |
| `Mode` | int | 0=MANUAL, 1=AUTO, 2=SCHEDULE |
| `ManualSwitch` | int | 0/1 pump switch in MANUAL mode (synced with local button) |
| `Threshold` | int | Moisture threshold (%) used in AUTO mode (also set by Encoder #1) |
| `PumpSeconds` | int | Pump run time (s) used in AUTO and SCHEDULE (also set by Encoder #2) |
| `Schedule/Date` | string | `YYYY-MM-DD` |
| `Schedule/Time` | string | `HH:MM` |

> Note: The firmware writes the key as **`Soid`** (as implemented in code).

---

## Firmware Behavior Summary

### MANUAL (Mode = 0)
- Pump is toggled directly by:
  - Local **Pump button**
  - Web App switch (`ManualSwitch`)
- Two-way sync keeps both interfaces consistent.

### AUTO (Mode = 1)
- If `Soid >= Threshold`: pump stays OFF.
- If `Soid < Threshold`: pump runs for `PumpSeconds` then stops.
- Cooldown is applied to prevent immediate re-triggering.

### SCHEDULE (Mode = 2)
- When current date/time matches `Schedule/Date` and `Schedule/Time`:
  - Pump runs for `PumpSeconds`.
- A de-duplication key prevents repeated triggering within the same scheduled minute.

### Low-Water Protection
- If `Volume < 200 ml`:
  - Pump is forced OFF and blocked from running.
  - `ManualSwitch` is pushed back to `0` via Firebase.
  - Warning LED blinks **500 ms ON / 500 ms OFF**.

---

## Water Volume Estimation Model

1. **Distance measurement** (HC-SR04):
   - Trigger pulse: 10 µs
   - Echo duration measured with `pulseIn()`
   - `distanceCm = duration * SOUND_SPEED / 2`  
     where `SOUND_SPEED = 0.034 cm/µs`

2. **Water height**:
   - `h = CUP_HEIGHT - distanceCm`
   - Clamped to `0 .. MAX_H`

3. **Container radius model** (frustum-like):
   - `radius = rBottom + (rTop - rBottom) * (h / MAX_H)`
   - `rBottom = 3.0 cm`, `rTop = 5.5 cm`

4. **Volume formula + scaling**:
   - `volume = (1/3) * PI * h * (rBottom^2 + 3*rCurrent + rCurrent^2)`
   - Scaled to ~800 ml using a precomputed `volumeMax` at `MAX_H`.

5. **Low-water threshold**:
   - `lowWater = (volume < 200)`

---

## Setup Guide

### 1) Firebase
1. Create a Firebase project.
2. Enable **Realtime Database**.
3. Create base path: `/Var`
4. Add/initialize keys:
   - `Mode`, `ManualSwitch`, `Threshold`, `PumpSeconds`
   - `Schedule/Date`, `Schedule/Time`

### 2) Arduino IDE Dependencies
Install the following libraries:
- `FirebaseESP32`
- `Adafruit_GFX`
- `Adafruit_SH110X`

### 3) Configure Credentials
In the firmware source, set:
- `WIFI_SSID`, `WIFI_PASSWORD`
- `FIREBASE_HOST`, `FIREBASE_AUTH`
- `path = "/Var"`

### 4) Upload (Arduino IDE)
1. Select board: **ESP32 Dev Module** (commonly compatible with NodeMCU/LuaNode32).
2. Select the correct COM port.
3. Upload the firmware.
4. Open Serial Monitor at **115200 baud** and verify:
   - Wi-Fi connects
   - Sensor values update
   - Firebase read/write works

### 5) Web App
The Web App UI (HTML/CSS/JS) remains unchanged from your latest version and should:
- Read from `/Var`
- Write to `/Var`
- Reflect two-way sync with ESP32 and local controls

---

## Troubleshooting

### Random resets / brownout / instability
- Relay and motor noise can cause resets. Improve wiring, grounding, and decoupling near the ESP32 board if needed.

### Firebase does not update
- Verify Wi-Fi connectivity.
- Validate `FIREBASE_HOST`, `FIREBASE_AUTH`, and `path`.
- Ensure database rules allow read/write for testing.
- Keep Firebase operations consolidated (current implementation uses a single `FirebaseData`, which is appropriate).

### HC-SR04 readings are unstable
- Ensure ECHO signal level is appropriate for ESP32 input.
- Avoid vibration and ensure sensor mounting is stable.

### OLED not detected
- Confirm I2C wiring (SDA=21, SCL=22) and address (`0x3C` / `0x3D`).
- Verify I2C pull-up voltage level and apply level shifting if required.

### KY-040 jitter / reversed direction
- Swap CLK/DT if direction is reversed.
- Apply additional filtering/debounce if needed.
- Verify signal levels on CLK/DT/SW before ESP32 GPIO input.