# Smart Irrigation System (ESP32 + Firebase + Web App)

A smart irrigation project using **ESP32**, **capacitive soil moisture sensor**, **HC-SR04 water level/volume**, **relay + pump**, **SH1106 OLED**, and **Firebase Realtime Database**.  
Control + monitoring via **Firebase Realtime Database** (web ↔ ESP32 **two-way sync**), with **2 physical buttons** and **2 rotary encoders (KY-040)**.

---

## Features

- **Soil moisture monitoring (Soid %)** using Capacitive Soil Moisture Sensor v1.2 (AO → ADC).
- **Water remaining estimation (Volume ml)** using HC-SR04 and volume calculation.
- **OLED SH1106 display**:
  - Soil moisture (%)
  - Water volume (ml)
  - Bottom fixed line shows current settings, e.g.: `Th:<threshold>%  T:<pump>s  M:<mode>`
- **Low water protection**:
  - If `Volume < 200 ml` → **pump is blocked** (forced OFF).
  - Warn LED blinks **500ms ON / 500ms OFF** (same logic as original).
- **3 watering modes** (web/Firebase + physical Mode button):
  1. **MANUAL** (Mode = 0)  
     - Pump is direct **ON/OFF** (no threshold, no timer).
     - **Physical Pump button** and **web switch** work in parallel and stay **synced**.
  2. **AUTO** (Mode = 1)  
     - If `Soid < Threshold` → pump runs for `PumpSeconds` then stops.
     - Repeats after a cooldown delay if still dry (configurable in code).
  3. **SCHEDULE** (Mode = 2)  
     - Runs at scheduled date/time for `PumpSeconds` then stops.
- **2× KY-040 rotary encoders (physical tuning)**:
  - Encoder #1: adjust **Threshold (%)** (AUTO mode)
  - Encoder #2: adjust **PumpSeconds (s)** (AUTO + SCHEDULE)
  - Values can be pushed to Firebase so web updates too (two-way sync).
- **Serial Monitor output** every **5 seconds** (same key values as web), with separator `----`.

---

## Hardware

### ESP32 Pins (baseline)

| Module | Signal | ESP32 GPIO |
|------|--------|------------|
| Relay (Pump) | IN | GPIO27 |
| Pump LED | LED | GPIO16 |
| Button #1 (Pump toggle) | INPUT_PULLUP | GPIO32 |
| Button #2 (Mode cycle) | INPUT_PULLUP | GPIO33 |
| HC-SR04 | TRIG | GPIO5 |
| HC-SR04 | ECHO | GPIO18 |
| Low water warn LED | LED | GPIO17 |
| Soil sensor v1.2 | AO | GPIO34 |
| OLED SH1106 I2C | SDA | GPIO21 |
| OLED SH1106 I2C | SCL | GPIO22 |

### KY-040 wiring (example)

> You can change GPIOs to match your code. Below is a safe/common suggestion (use **ADC-free** pins and avoid strapping pins).

**Encoder #1 (Threshold):**
- VCC → **3V3**
- GND → GND
- CLK → GPIO25
- DT  → GPIO26
- SW  → (optional) GPIO14

**Encoder #2 (PumpSeconds):**
- VCC → **3V3**
- GND → GND
- CLK → GPIO12
- DT  → GPIO13
- SW  → (optional) GPIO15

> If your firmware uses different pins, update this table to match.

---

## Power notes (important)

- **ESP32 is 3.3V logic**.
- **HC-SR04 ECHO is 5V** if HC-SR04 is powered by 5V → you **must use a level shifter / voltage divider** on ECHO to protect ESP32.
- Soil sensor v1.2 can be powered by **5V**, but its AO voltage must still be safe for ESP32 ADC.  
  (Most versions output within 0–3.3V, but verify. If it can exceed 3.3V, use a divider.)
- **KY-040**: recommended power **3V3** so its outputs are 3.3V-safe.
  - If you power KY-040 at **5V**, then its CLK/DT/SW signals may go up to 5V → **level shifting/divider required**.
  - If you already level-shifted to 3.3V, you typically **don’t need to change code**.

---

## Firebase Realtime Database Structure

Base path: **`/Var`**

| Key | Type | Description |
|-----|------|-------------|
| `Soid` | int | Soil moisture (%) from ESP32 |
| `Volume` | float / int | Water remaining (ml) |
| `Mode` | int | 0=MANUAL, 1=AUTO, 2=SCHEDULE |
| `ManualSwitch` | int | 0/1 (MANUAL ON/OFF), synced with physical pump button |
| `Threshold` | int | Soil threshold (%) used in AUTO (also adjustable by KY-040 #1) |
| `PumpSeconds` | int | Pump run time (seconds) used in AUTO + SCHEDULE (also adjustable by KY-040 #2) |
| `Schedule/Date` | string | `YYYY-MM-DD` |
| `Schedule/Time` | string | `HH:MM` |

---

## Steps

1. **Wire the hardware**
   - Connect sensors, relay, OLED, 2 buttons, and 2 encoders as listed.
   - Ensure **common GND** between ESP32, sensors, and relay module.
   - If pump/relay uses a separate supply, connect grounds together.

2. **Create Firebase Realtime Database**
   - Create a Firebase project
   - Enable **Realtime Database**
   - Create (or ensure) base path: `/Var`
   - Add keys: `Mode`, `ManualSwitch`, `Threshold`, `PumpSeconds`, `Schedule/Date`, `Schedule/Time`

3. **Install required libraries**
   - `FirebaseESP32`
   - `Adafruit_GFX`
   - `Adafruit_SH110X`

4. **Configure credentials**
   - In ESP32 firmware, set:
     - `WIFI_SSID`, `WIFI_PASSWORD`
     - `FIREBASE_HOST`, `FIREBASE_AUTH`
     - `path = "/Var"`

5. **Upload firmware to ESP32**
   - Open Serial Monitor at **115200 baud**
   - Confirm the device connects to WiFi, reads sensors, and updates Firebase.

6. **Open the web app**
   - The web app reads/writes `/Var` keys to control modes, threshold, and pump seconds.
   - Verify two-way sync:
     - Web changes → ESP32 reacts
     - Physical buttons/encoders → Firebase updates → web reflects changes

---

## Volume calculation

This project estimates remaining water volume (ml) using an HC-SR04 ultrasonic sensor and a container model.

### 1) Distance from HC-SR04
- Trigger pulse: 10 µs
- Measure echo duration using `pulseIn(ECHO, HIGH, timeout)`
- Convert to distance (cm):

`distanceCm = duration * SOUND_SPEED / 2`

Where:
- `SOUND_SPEED = 0.034` cm/µs

### 2) Water height
Water height in the container:

`h = CUP_HEIGHT - distanceCm`

Clamp to valid range:
- if h < 0 → h = 0
- if h > MAX_H → h = MAX_H

### 3) Radius model
The container is modeled as a frustum-like shape where radius changes with height.

`radius = rBottom + (rTop - rBottom) * (h / MAX_H)`

Used values in code:
- rBottom = 3.0 (cm)
- rTop    = 5.5 (cm)

### 4) Volume formula + scaling
Using the same formula as original code:

`volume = (1/3) * PI * h * (rBottom^2 + 3*rCurrent + rCurrent^2)`

Then scale to a maximum volume:

`volume = volume * (800 / volumeMax)`

Where `volumeMax` is computed at MAX_H using rTop.

### 5) Low water logic
Low water is detected exactly as original:

`lowWater = (volume < 200)`

If `lowWater` is true:
- Pump is blocked (forced OFF)
- Warn LED blinks 500ms ON / 500ms OFF

---

## Getting Started

1. Create Firebase Realtime Database.
2. Set WiFi/Firebase credentials in code:
   - `WIFI_SSID`, `WIFI_PASSWORD`
   - `FIREBASE_HOST`, `FIREBASE_AUTH`
3. Upload code to ESP32.
4. Open Serial Monitor (115200 baud) to verify values.
5. Control via Web App (reads/writes `/Var`) and verify two-way sync with physical controls.

---

## Troubleshooting

### ESP32 resets / watchdog
- Check power supply (pump relay noise can reset ESP32).
- Use separate power for pump and ESP32 with common GND.
- Add decoupling capacitors near ESP32 + relay supply.
- Keep wiring short, separate pump power lines from signal lines.

### No Firebase updates
- Confirm WiFi connected.
- Check Firebase host/auth and database path `/Var`.
- Ensure database rules allow read/write for your test setup.
- Avoid calling Firebase set/get from multiple tasks with the same `FirebaseData` object.

### HC-SR04 unstable readings
- Ensure ECHO level shifting if HC-SR04 is powered at 5V.
- Keep sensor stable, avoid vibration.
- Make sure container geometry constants match your real container.

### Soil sensor values not stable
- Recalibrate `rawDry` and `rawWet`.
- If AO can exceed 3.3V, use a voltage divider.

### KY-040 “jumping” / reversed direction
- Swap CLK/DT wires if direction is inverted.
- Add simple debounce (software) if needed.
- Prefer powering KY-040 at 3.3V to avoid level shifting issues.

---