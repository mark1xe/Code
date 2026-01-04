# Smart Irrigation System (ESP32 + Firebase + Web App)

A smart irrigation project using **ESP32**, **capacitive soil moisture sensor**, **HC-SR04 water level/volume**, **relay + pump**, **SH1106 OLED**, and **Firebase Realtime Database**.  
The web app allows selecting watering modes and adjusting parameters in real time.

---

## Features

- **Soil moisture monitoring (Soid %)** using Capacitive Soil Moisture Sensor v1.2 (AO → ADC).
- **Water remaining estimation (Volume ml)** using HC-SR04 and volume calculation.
- **OLED SH1106 display**:
  - Soil moisture (%)
  - Water volume (ml)
  - Bottom fixed line: `Th:<threshold>%  M:<mode>`
- **Low water protection**:
  - If `Volume < 200 ml` → **pump is blocked** (forced OFF).
  - Warn LED blinks **500ms ON / 500ms OFF** (same logic as original).
- **3 watering modes controlled from web/Firebase**:
  1. **MANUAL** (Mode = 0)  
     - Pump is direct **ON/OFF** (no threshold, no timer).
     - **Physical button** and **web switch** work in parallel and stay **synced**.
  2. **AUTO** (Mode = 1)  
     - If `Soid < Threshold` → pump runs for `PumpSeconds` then stops.
     - Repeats after a cooldown delay if still dry (configurable in code).
  3. **SCHEDULE** (Mode = 2)  
     - Runs at scheduled date/time for `PumpSeconds` then stops.
- **Serial Monitor output** every **5 seconds** (same values as web):
  - Soid
  - Volume
  - Threshold
  - Mode
  - Separator: `----`

---

## Hardware

### ESP32 Pins

    Relay IN     -> GPIO27
    Button       -> GPIO32 (INPUT_PULLUP, button to GND)
    Pump LED     -> GPIO16

    HC-SR04:
      VCC  -> 5V
      GND  -> GND
      TRIG -> GPIO5
      ECHO -> GPIO18

    Warn LED     -> GPIO17

    Soil sensor (Capacitive v1.2):
      VCC  -> 5V
      GND  -> GND
      AO   -> GPIO34 (ADC1)

    OLED SH1106 I2C:
      SDA -> GPIO21
      SCL -> GPIO22
      VCC -> 5V
      GND -> GND

### Power notes (important)

- **ESP32 is 3.3V logic**.
- **HC-SR04 ECHO is 5V** if HC-SR04 is powered by 5V → you **must use a level shifter / voltage divider** on ECHO to protect ESP32.
- Soil sensor v1.2 can be powered by **5V**, but its AO voltage must still be safe for ESP32 ADC.  
  (Most versions output within 0–3.3V, but you should verify with a multimeter. If it can exceed 3.3V, use a divider.)

---

## Software Requirements

- Arduino IDE or PlatformIO (ESP32 core installed)
- Libraries:
  - `FirebaseESP32`
  - `WiFi` (built-in)
  - `Adafruit_GFX`
  - `Adafruit_SH110X`

---

## Firebase Realtime Database Structure

Base path: **`/Var`**

| Key               | Type          | Description                                       |
|-------------------|---------------|---------------------------------------------------|
| `Soid`            | int           | Soil moisture (%) from ESP32                      |
| `Volume`          | float / int   | Water remaining (ml)                              |
| `Mode`            | int           | 0=MANUAL, 1=AUTO, 2=SCHEDULE                      |
| `ManualSwitch`    | int           | 0/1 (MANUAL ON/OFF), synced with physical button  |
| `Threshold`       | int           | Soil threshold (%) used in AUTO                   |
| `PumpSeconds`     | int           | Pump run time (seconds) used in AUTO + SCHEDULE   |
| `Schedule/Date`   | string        | `YYYY-MM-DD`                                      |
| `Schedule/Time`   | string        | `HH:MM`                                           |

> The ESP32 periodically uploads `Soid` and `Volume` and reads `Mode/ManualSwitch/Threshold/PumpSeconds/Schedule`.

---

## How It Works

### Mode 0 — MANUAL
- Web toggle (`ManualSwitch`) turns pump ON/OFF immediately.
- Physical button toggles pump ON/OFF and **updates `ManualSwitch` on Firebase** for full sync.
- `Threshold` and `PumpSeconds` are not used.

### Mode 1 — AUTO
- If `Soid < Threshold` and not currently pumping → start pump for `PumpSeconds`.
- Stop pump strictly by time.
- If still dry → wait cooldown → pump again.
- If soil becomes wet enough (`Soid >= Threshold`) → stops and resets for future cycles.

### Mode 2 — SCHEDULE
- At the exact scheduled date/time → start pump for `PumpSeconds` then stop.
- Prevents repeated triggering within the same minute.

### Low water block
- If `Volume < 200 ml`:
  - Pump is forced OFF
  - ManualSwitch is set to 0
  - Warn LED blinks continuously

---

## Calibration

### Soil sensor calibration
Edit in code:
```cpp
int rawDry = 2650;  // sensor in air (dry)
int rawWet = 1150;  // sensor in water (wet)
# Code

