# Smart Irrigation System (ESP32 + Firebase + Web App)

A smart irrigation project using **ESP32**, **capacitive soil moisture sensor module**, **HC-SR04 module**, **relay + pump**, **SH1106 OLED module**, and **Firebase Realtime Database**.  
Control + monitoring via **Firebase Realtime Database** (web ↔ ESP32 **two-way sync**), with **2 physical buttons** and **2 rotary encoders (KY-040)**.

---

## Features

- **Soil moisture monitoring (Soid %)** using capacitive soil moisture sensor module (AO → ADC).
- **Water remaining estimation (Volume ml)** using HC-SR04 module and volume calculation.
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
- **2× KY-040 rotary encoder modules (physical tuning)**:
  - Encoder #1: adjust **Threshold (%)** (AUTO mode)
  - Encoder #2: adjust **PumpSeconds (s)** (AUTO + SCHEDULE)
  - Values can be pushed to Firebase so web updates too (two-way sync).
- **Serial Monitor output** every **5 seconds** (same key values as web), with separator `----`.

---

## Hardware

### ESP32 pins (baseline)

| Module | Signal | ESP32 GPIO |
|------|--------|------------|
| Relay (Pump) | IN | GPIO27 |
| Pump LED | LED | GPIO16 |
| Button #1 (Pump toggle) | INPUT_PULLUP | GPIO32 |
| Button #2 (Mode cycle) | INPUT_PULLUP | GPIO33 |
| HC-SR04 | TRIG | GPIO5 |
| HC-SR04 | ECHO | GPIO18 |
| Low water warn LED | LED | GPIO17 |
| Soil moisture module | AO | GPIO34 |
| OLED SH1106 I2C | SDA | GPIO21 |
| OLED SH1106 I2C | SCL | GPIO22 |

---

## Wiring (all modules powered at 5V)

> You said all sensors/encoders are **modules** and work stable at **5V**.  
> ESP32 GPIO are **3.3V only**, so **any 5V digital signal to ESP32 must be level-shifted / divided**.  
> ADC input must also be ≤ 3.3V.

### 1) Power
- Use a stable **5V supply** for modules (and relay module if compatible).
- **ESP32 can be powered by 5V** via VIN/5V pin (depending on your board).
- **Common GND is mandatory** between ESP32 and all modules.

### 2) HC-SR04 module (5V)
- VCC → **5V**
- GND → GND
- TRIG → GPIO5 (3.3V output from ESP32 is OK to drive TRIG)
- **ECHO → GPIO18 через divider/level-shifter** (important)
  - Simple divider example: **ECHO -> 20k -> GPIO18**, GPIO18 -> **10k -> GND** (≈3.3V)

### 3) Soil moisture module (5V)
- VCC → **5V**
- GND → GND
- **AO → GPIO34 (ADC)**  
  - If AO output can exceed 3.3V: add a divider (same idea as above).
  - Many modules output ≤3.3V, but verify once with a multimeter.

### 4) SH1106 OLED module (5V)
- VCC → **5V** (module with onboard regulator)
- GND → GND
- SDA → GPIO21
- SCL → GPIO22  
  - Most OLED I2C modules can work at 3.3V logic even if powered at 5V, but if yours pulls SDA/SCL up to 5V, use:
    - **I2C level shifter** (recommended) or
    - Change pullups to 3.3V (if you know what you’re doing).

### 5) KY-040 encoder modules (5V)
Each KY-040 has: **VCC, GND, CLK, DT, SW**.

**Encoder #1 (Threshold):**
- VCC → **5V**
- GND → GND
- CLK → (your GPIO)
- DT  → (your GPIO)
- SW  → (optional GPIO)

**Encoder #2 (PumpSeconds):**
- VCC → **5V**
- GND → GND
- CLK → (your GPIO)
- DT  → (your GPIO)
- SW  → (optional GPIO)

**Important:** KY-040 outputs are typically **5V** when powered at 5V →  
Use **level shifting / dividers on CLK/DT/SW** before ESP32 pins.

Divider example per signal line:
- Signal -> **20k** -> ESP32 GPIO
- ESP32 GPIO -> **10k** -> GND

### 6) Buttons (direct to ESP32)
Buttons using `INPUT_PULLUP` should be wired:
- GPIO32 → button → GND
- GPIO33 → button → GND

---

## Firebase Realtime Database Structure

Base path: **`/Var`**

| Key | Type | Description |
|-----|------|-------------|
| `Soid` | int | Soil moisture (%) from ESP32 |
| `Volume` | float / int | Water remaining (ml) |
| `Mode` | int | 0=MANUAL, 1=AUTO, 2=SCHEDULE |
| `ManualSwitch` | int | 0/1 (MANUAL ON/OFF), synced with physical pump button |
| `Threshold` | int | Soil threshold (%) used in AUTO (also from KY-040 #1) |
| `PumpSeconds` | int | Pump run time (seconds) used in AUTO + SCHEDULE (also from KY-040 #2) |
| `Schedule/Date` | string | `YYYY-MM-DD` |
| `Schedule/Time` | string | `HH:MM` |

---

## Steps

1. **Wire the hardware (5V modules)**
   - Power all modules with **5V**.
   - Connect **common GND**.
   - Add **level shifting/dividers** for any module signal lines that output 5V into ESP32 GPIO.

2. **Create Firebase Realtime Database**
   - Create a Firebase project
   - Enable **Realtime Database**
   - Ensure base path: `/Var`
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
   - Confirm WiFi connects, sensor values update, and Firebase updates.

6. **Open the web app**
   - The web app reads/writes `/Var` keys.
   - Verify two-way sync:
     - Web → ESP32 reacts
     - Physical buttons/encoders → Firebase updates → web shows changes

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
- Pump relay noise can reset ESP32 → use separate power for pump + ESP32, keep common GND.
- Add capacitors near ESP32 5V input (e.g., 470µF + 0.1µF).
- Keep pump wires away from signal wires.

### No Firebase updates
- Confirm WiFi connected.
- Check Firebase host/auth and database path `/Var`.
- Ensure database rules allow read/write for testing.
- Keep Firebase calls in one task (avoid multiple tasks writing with same FirebaseData).

### HC-SR04 unstable readings
- Ensure **ECHO is level-shifted** into ESP32.
- Keep sensor stable, avoid vibration.

### OLED not detected (I2C)
- Confirm address (0x3C / 0x3D).
- If OLED module pulls SDA/SCL up to 5V, add an I2C level shifter.

### KY-040 wrong / noisy steps
- If direction is reversed → swap CLK/DT.
- If jittery → add software debounce / filtering.
- Ensure CLK/DT/SW are **level-shifted** into ESP32 when KY-040 is powered at 5V.