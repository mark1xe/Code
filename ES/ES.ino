/*
  COMBINED: Pump + Button + Soil Moisture + Water Volume + Warn LED + OLED SH1106 + WebServer

  Pins:
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
*/

#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time.h"

// ===== WiFi/FireBase Define ====
#define WIFI_SSID "Thanh Thuy"
#define WIFI_PASSWORD "thanhthuy"

#define FIREBASE_HOST "https://project2-cd9c9-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "tsnWspwpI5U5nuM1H5FfVbFvBfJgS3ASCravKsg53y9"  
#undef FIREBASE_AUTH
#define FIREBASE_AUTH "tsnWspwpI5U5nuM1H5FfVbFJgS3ASCravKsg53y9"

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;
String path = "/Var";

// --- Serial mutex ---
SemaphoreHandle_t serialMutex = nullptr;
static inline void SerialLock() { if (serialMutex) xSemaphoreTake(serialMutex, portMAX_DELAY); }
static inline void SerialUnlock() { if (serialMutex) xSemaphoreGive(serialMutex); }

// ===== Pump + Button =====
#define RELAY_PIN   27
#define BUTTON_PIN  32
#define LED_PIN     16

volatile bool relayState = false;     // pump OFF at start
bool lastButtonState = HIGH;          // INPUT_PULLUP idle = HIGH
bool buttonState = HIGH;

unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

// ===== Soil =====
#define SOIL_PIN 34
const int samples = 20;
const int sampleDelayMs = 10;

int rawDry = 2650;
int rawWet = 1150;
volatile int soilPercent = 0;

// ===== Water Volume + Warn LED =====
#define TRIG_PIN      5
#define ECHO_PIN      18
#define WARN_LED_PIN  17

#define SOUND_SPEED   0.034
#define CUP_HEIGHT    14.0
#define MAX_H         11.0

volatile bool  lowWater = false;       // true when volume < 200
volatile float waterVolumeMl = 0.0;

// ===== OLED =====
Adafruit_SH1106G display(128, 64, &Wire, -1);

// ===== Firebase control vars =====
// Mode: 0=MANUAL, 1=AUTO, 2=SCHEDULE
volatile int fbMode = 0;

// Threshold: apply to AUTO only
volatile int fbThreshold = 40;

// PumpSeconds: apply to AUTO + SCHEDULE only
volatile int fbPumpSeconds = 10;

// ManualSwitch: use in MANUAL mode as ON/OFF state
volatile int fbManualSwitch = 0;

String fbSchDate = "";   // YYYY-MM-DD
String fbSchTime = "";   // HH:MM

// ===== Timer for mode 2 & 3 =====
bool pumpTimedRunning = false;
uint32_t pumpStopAtMs = 0;

// ===== AUTO repeat control (AUTO = fbMode 1) =====
const uint32_t AUTO_COOLDOWN_MS = 30000;   // nghỉ 30s giữa các lần tưới auto
uint32_t autoNextAllowedMs = 0;            // thời điểm được phép tưới auto lần tiếp theo

// Track manual switch changes (avoid web overriding physical toggle)
int lastManualSwitchApplied = -1;

// Prevent schedule re-trigger
String lastScheduleKey = "";

// ===== NEW: ManualSwitch sync from physical button -> Firebase (2-way sync) =====
volatile bool manualSwitchDirty = false;   // when true, firebaseTask must push ManualSwitch
volatile int  manualSwitchPending = 0;     // value to push (0/1)

// ===== Task prototypes =====
void waterTask(void *pvParameters);
void warnLedTask(void *pvParameters);
void soilTask(void *pvParameters);
void displayTask(void *pvParameters);
void firebaseTask(void *pvParameters);
void controlTask(void *pvParameters);
void serialTask(void *pvParameters);

// ===== Helpers =====
int readSoilRawAvg() {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(SOIL_PIN);
    delay(sampleDelayMs);
  }
  return (int)(sum / samples);
}

static inline const char* modeText3(int m) {
  if (m == 0) return "MAN";
  if (m == 1) return "AUT";
  if (m == 2) return "SCH";
  return "N/A";
}

static inline const char* modeTextFull(int m) {
  if (m == 0) return "MANUAL";
  if (m == 1) return "AUTO";
  if (m == 2) return "SCHEDULE";
  return "N/A";
}

void applyRelay(bool on) {
  relayState = on;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  digitalWrite(LED_PIN, relayState ? HIGH : LOW);
}

void startTimedPump(int sec) {
  if (sec < 1) sec = 1;
  pumpTimedRunning = true;
  pumpStopAtMs = millis() + (uint32_t)sec * 1000UL;
  applyRelay(true);
}

void stopTimedPump() {
  pumpTimedRunning = false;
  applyRelay(false);
}

void setupTimeNTP() {
  // GMT+7
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}

String todayDate() {
  struct tm t;
  if (!getLocalTime(&t, 2000)) return "";
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

String nowTimeHM() {
  struct tm t;
  if (!getLocalTime(&t, 2000)) return "";
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  serialMutex = xSemaphoreCreateMutex();

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  applyRelay(false);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(WARN_LED_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(WARN_LED_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  Wire.begin(21, 22);
  if (!display.begin(0x3C, true)) {
    while (1) delay(10);
  }

  // ===== WiFi connect =====
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    SerialLock(); Serial.print("."); SerialUnlock();
  }
  SerialLock(); Serial.println("\nWiFi connected."); SerialUnlock();

  setupTimeNTP();

  // ===== Firebase connect =====
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Tasks
  xTaskCreate(waterTask,    "Water Task",    3000, NULL, 1, NULL);
  xTaskCreate(warnLedTask,  "Warn LED Task", 1000, NULL, 1, NULL);
  xTaskCreate(soilTask,     "Soil Task",     2500, NULL, 1, NULL);
  xTaskCreate(displayTask,  "OLED Task",     2500, NULL, 1, NULL);

  xTaskCreate(firebaseTask, "Firebase Task", 8192, NULL, 1, NULL);
  xTaskCreate(controlTask,  "Control Task",  4096, NULL, 1, NULL);
  xTaskCreate(serialTask,   "Serial Task",   3072, NULL, 1, NULL);
}

void loop() {
  // Physical button works ONLY in MANUAL mode (fbMode==0) and in parallel with web.
  if (fbMode != 0) {
    lastButtonState = digitalRead(BUTTON_PIN);
    return;
  }

  // Low water blocks pumping
  if (lowWater) {
    lastButtonState = digitalRead(BUTTON_PIN);
    return;
  }

  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonState) lastDebounceTime = millis();

  if (millis() - lastDebounceTime > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;

      // Toggle on RELEASE (HIGH)
      if (buttonState == HIGH) {
        bool newState = !relayState;
        applyRelay(newState);

        // Update local ManualSwitch immediately
        fbManualSwitch = newState ? 1 : 0;

        // NEW: mark to push to Firebase in firebaseTask (avoid multi-thread Firebase calls)
        manualSwitchPending = fbManualSwitch;
        manualSwitchDirty = true;

        SerialLock();
        Serial.println("----");
        Serial.print("Physical button -> ManualSwitch pending = ");
        Serial.println(manualSwitchPending);
        SerialUnlock();
      }
    }
  }

  lastButtonState = reading;
}

// ===== Water Task =====
void waterTask(void *pvParameters) {
  float volumeMax = (1.0 / 3.0) * PI * MAX_H *
                    (sq(3.0) + 3.0 * 5.5 + sq(5.5));

  while (1) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration == 0) {
      vTaskDelay(300 / portTICK_PERIOD_MS);
      continue;
    }

    float distanceCm = duration * SOUND_SPEED / 2.0;

    float h = CUP_HEIGHT - distanceCm;
    if (h < 0) h = 0;
    if (h > MAX_H) h = MAX_H;

    float radius = 3.0 + (5.5 - 3.0) * (h / MAX_H);

    float volume = (1.0 / 3.0) * PI * h *
                   (sq(3.0) + 3.0 * radius + sq(radius));
    volume = volume * (800.0 / volumeMax);

    waterVolumeMl = volume;

    // KEEP ORIGINAL LOW WATER LOGIC
    lowWater = (volume < 200);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===== Warn LED Task =====
void warnLedTask(void *pvParameters) {
  while (1) {
    if (lowWater) {
      digitalWrite(WARN_LED_PIN, HIGH);
      vTaskDelay(500 / portTICK_PERIOD_MS);
      digitalWrite(WARN_LED_PIN, LOW);
      vTaskDelay(500 / portTICK_PERIOD_MS);
    } else {
      digitalWrite(WARN_LED_PIN, LOW);
      vTaskDelay(200 / portTICK_PERIOD_MS);
    }
  }
}

// ===== Soil Task =====
void soilTask(void *pvParameters) {
  while (1) {
    int raw = readSoilRawAvg();
    int moisturePercent = map(raw, rawDry, rawWet, 0, 100);
    moisturePercent = constrain(moisturePercent, 0, 100);
    soilPercent = moisturePercent;

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===== OLED Task =====
void displayTask(void *pvParameters) {
  while (1) {
    int sp = soilPercent;
    int th = fbThreshold;
    int mode = fbMode;
    float vol = waterVolumeMl;

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Soil:");
    display.setTextSize(2);
    display.setCursor(42, 0);
    display.print(sp);
    display.print("%");

    display.setTextSize(1);
    display.setCursor(0, 24);
    display.print("Water:");
    display.setTextSize(2);
    display.setCursor(54, 24);
    display.print((int)vol);
    display.print("ml");

    // Fixed last line
    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("Th:");
    display.print(th);
    display.print("%  M:");
    display.print(modeText3(mode));

    display.display();
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ===== Firebase Task =====
// write Soid/Volume every 5s
// read control values every 2s
// NEW: push ManualSwitch if dirty (from physical button)
void firebaseTask(void *pvParameters) {
  uint32_t lastWrite = 0;
  uint32_t lastRead  = 0;

  while (1) {
    if (Firebase.ready()) {

      // NEW: push ManualSwitch from physical button (2-way sync)
      if (manualSwitchDirty) {
        int v = manualSwitchPending;
        if (Firebase.setInt(firebaseData, path + "/ManualSwitch", v)) {
          manualSwitchDirty = false;     // done
          // keep fbManualSwitch consistent
          fbManualSwitch = v;

          SerialLock();
          Serial.println("----");
          Serial.print("Synced to Firebase ManualSwitch = ");
          Serial.println(v);
          SerialUnlock();
        } else {
          SerialLock();
          Serial.println("----");
          Serial.print("Failed sync ManualSwitch: ");
          Serial.println(firebaseData.errorReason());
          SerialUnlock();
          // keep dirty = true => will retry next loop
        }
      }

      if (millis() - lastWrite >= 5000) {
        lastWrite = millis();
        Firebase.setInt(firebaseData, path + "/Soid", (int)soilPercent);
        Firebase.setFloat(firebaseData, path + "/Volume", (float)waterVolumeMl);
      }

      if (millis() - lastRead >= 2000) {
        lastRead = millis();

        if (Firebase.getInt(firebaseData, path + "/Mode")) {
          fbMode = firebaseData.intData();
        }
        if (Firebase.getInt(firebaseData, path + "/Threshold")) {
          fbThreshold = firebaseData.intData();
        }
        if (Firebase.getInt(firebaseData, path + "/PumpSeconds")) {
          fbPumpSeconds = firebaseData.intData();
        }

        // IMPORTANT: only read ManualSwitch from Firebase when NOT pending local push
        if (!manualSwitchDirty) {
          if (Firebase.getInt(firebaseData, path + "/ManualSwitch")) {
            fbManualSwitch = firebaseData.intData();
          }
        }

        if (Firebase.getString(firebaseData, path + "/Schedule/Date")) {
          fbSchDate = firebaseData.stringData();
        }
        if (Firebase.getString(firebaseData, path + "/Schedule/Time")) {
          fbSchTime = firebaseData.stringData();
        }
      }
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ===== Control Task =====
void controlTask(void *pvParameters) {
  while (1) {
    int mode = fbMode;

    // Low water: block everything
    if (lowWater) {
      if (pumpTimedRunning) stopTimedPump();
      if (relayState) applyRelay(false);

      if (fbManualSwitch != 0) {
        fbManualSwitch = 0;
        manualSwitchPending = 0;
        manualSwitchDirty = true; // push OFF to Firebase
      }

      vTaskDelay(200 / portTICK_PERIOD_MS);
      continue;
    }

    // Stop timed pump when time reached
    if (pumpTimedRunning && millis() >= pumpStopAtMs) {
      stopTimedPump();

      if (mode == 1) {
        autoNextAllowedMs = millis() + AUTO_COOLDOWN_MS;
      }
    }

    if (mode == 0) {
      // MANUAL: web switch direct ON/OFF
      // Apply ONLY when changed to avoid "web overriding"
      if (fbManualSwitch != lastManualSwitchApplied) {
        lastManualSwitchApplied = fbManualSwitch;
        applyRelay(fbManualSwitch == 1);
      }

      if (pumpTimedRunning) stopTimedPump();
      autoNextAllowedMs = 0;
    }
    else if (mode == 1) {
      // AUTO: Threshold decides WHEN to water, PumpSeconds decides HOW LONG.
      // Stop ALWAYS by time. If still dry, water again after cooldown.
      lastManualSwitchApplied = -1;

      int soil = soilPercent;

      if (soil >= fbThreshold) {
        if (!pumpTimedRunning && relayState) applyRelay(false);
        autoNextAllowedMs = 0;
      } else {
        if (!pumpTimedRunning && (autoNextAllowedMs == 0 || millis() >= autoNextAllowedMs)) {
          startTimedPump(fbPumpSeconds);
        }
      }
    }
    else if (mode == 2) {
      // SCHEDULE: timed watering at date/time
      lastManualSwitchApplied = -1;
      autoNextAllowedMs = 0;

      if (!pumpTimedRunning && fbSchDate.length() == 10 && fbSchTime.length() == 5) {
        String d = todayDate();
        String t = nowTimeHM();

        if (d == fbSchDate && t == fbSchTime) {
          String key = d + " " + t;
          if (key != lastScheduleKey) {
            lastScheduleKey = key;
            startTimedPump(fbPumpSeconds);
          }
        }
      }

      if (!pumpTimedRunning && relayState) applyRelay(false);
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ===== Serial Task =====
void serialTask(void *pvParameters) {
  while (1) {
    int soil = soilPercent;
    int vol = (int)waterVolumeMl;
    int th = fbThreshold;
    int mode = fbMode;

    SerialLock();
    Serial.println("----");
    Serial.print("Soid: ");
    Serial.println(soil);
    Serial.print("Volume: ");
    Serial.println(vol);
    Serial.print("Threshold: ");
    Serial.println(th);
    Serial.print("Mode: ");
    Serial.println(modeTextFull(mode));
    SerialUnlock();

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}
