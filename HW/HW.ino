#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time.h"

// ===== WiFi / Firebase =====
#define WIFI_SSID     "minhphuong"
#define WIFI_PASSWORD "01010000"

#define FIREBASE_HOST "https://project2-cd9c9-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "tsnWspwpI5U5nuM1H5FfVbFJgS3ASCravKsg53y9"

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;
String path = "/Var";

// ===== Serial mutex =====
SemaphoreHandle_t serialMutex = nullptr;
static inline void SerialLock()   { if (serialMutex) xSemaphoreTake(serialMutex, portMAX_DELAY); }
static inline void SerialUnlock() { if (serialMutex) xSemaphoreGive(serialMutex); }

// ===== Pump + Buttons =====
#define RELAY_PIN     27
#define BUTTON_PIN_1  32   // Pump (manual)
#define BUTTON_PIN_2  33   // Mode
#define LED_PIN       16

volatile bool relayState = false;

// Debounce button 1
bool lastButtonState = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

// Debounce button 2
bool lastModeBtnState = HIGH;
bool modeBtnState = HIGH;
unsigned long lastModeDebounceTime = 0;
unsigned long modeDebounceDelay = 50;

// ===== Soil =====
#define SOIL_PIN 34
const int samples = 20;
const int sampleDelayMs = 10;

int rawDry = 2650;
int rawWet = 1150;
volatile int soilPercent = 0;

// ===== Water volume + warn LED =====
#define TRIG_PIN      5
#define ECHO_PIN      18
#define WARN_LED_PIN  17

#define SOUND_SPEED   0.034
#define CUP_HEIGHT    14.0
#define MAX_H         11.0

volatile bool  lowWater = false;
volatile float waterVolumeMl = 0.0;

// ===== OLED =====
Adafruit_SH1106G display(128, 64, &Wire, -1);

// ===== Firebase vars =====
// Mode: 0=MANUAL, 1=AUTO, 2=SCHEDULE
volatile int fbMode = 0;
volatile int fbThreshold = 40;     // AUTO only
volatile int fbPumpSeconds = 10;   // AUTO + SCHEDULE
volatile int fbManualSwitch = 0;   // MANUAL ON/OFF

String fbSchDate = "";  // YYYY-MM-DD
String fbSchTime = "";  // HH:MM

// ===== Timed pump =====
bool pumpTimedRunning = false;
uint32_t pumpStopAtMs = 0;

// AUTO cooldown
const uint32_t AUTO_COOLDOWN_MS = 30000;
uint32_t autoNextAllowedMs = 0;

int lastManualSwitchApplied = -1;
String lastScheduleKey = "";

// 2-way sync flags
volatile bool manualSwitchDirty = false;
volatile int  manualSwitchPending = 0;

volatile bool modeDirty = false;
volatile int  modePending = 0;

// NEW: encoder -> Firebase flags
volatile bool thresholdDirty = false;
volatile int  thresholdPending = 0;

volatile bool pumpSecDirty = false;
volatile int  pumpSecPending = 0;

// ===== 2x KY-040 =====
// Encoder #1: Threshold
#define ENC_TH_CLK 25
#define ENC_TH_DT  26
#define ENC_TH_SW  14

// Encoder #2: PumpSeconds
#define ENC_PS_CLK 19
#define ENC_PS_DT  23
#define ENC_PS_SW  13

volatile int encThStep = 1;   // 1 or 5
volatile int encPsStep = 1;   // 1 or 5

// ===== Tasks =====
void waterTask(void *pvParameters);
void warnLedTask(void *pvParameters);
void soilTask(void *pvParameters);
void displayTask(void *pvParameters);
void firebaseTask(void *pvParameters);
void controlTask(void *pvParameters);
void serialTask(void *pvParameters);
void encoderTask(void *pvParameters);

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
  digitalWrite(LED_PIN,  relayState ? HIGH : LOW);
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
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // GMT+7
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

// ===== Encoder decode (table) =====
static inline int8_t encStepFrom(uint8_t prevAB, uint8_t currAB) {
  static const int8_t table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };
  return table[(prevAB << 2) | currAB];
}

void setup() {
  Serial.begin(115200);
  delay(200);

  serialMutex = xSemaphoreCreateMutex();

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  pinMode(BUTTON_PIN_1, INPUT_PULLUP);
  pinMode(BUTTON_PIN_2, INPUT_PULLUP);

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

  // KY-040 pins
  pinMode(ENC_TH_CLK, INPUT_PULLUP);
  pinMode(ENC_TH_DT,  INPUT_PULLUP);
  pinMode(ENC_TH_SW,  INPUT_PULLUP);

  pinMode(ENC_PS_CLK, INPUT_PULLUP);
  pinMode(ENC_PS_DT,  INPUT_PULLUP);
  pinMode(ENC_PS_SW,  INPUT_PULLUP);

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    SerialLock(); Serial.print("."); SerialUnlock();
  }
  SerialLock(); Serial.println("\nWiFi connected."); SerialUnlock();

  setupTimeNTP();

  // Firebase
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Tasks
  xTaskCreate(waterTask,    "Water",    3000, NULL, 1, NULL);
  xTaskCreate(warnLedTask,  "WarnLED",  1000, NULL, 1, NULL);
  xTaskCreate(soilTask,     "Soil",     2500, NULL, 1, NULL);
  xTaskCreate(displayTask,  "OLED",     2500, NULL, 1, NULL);

  xTaskCreate(firebaseTask, "Firebase", 8192, NULL, 1, NULL);
  xTaskCreate(controlTask,  "Control",  4096, NULL, 1, NULL);
  xTaskCreate(serialTask,   "Serial",   3072, NULL, 1, NULL);

  xTaskCreate(encoderTask,  "Encoders", 3072, NULL, 2, NULL);
}

void loop() {
  // Button 2: Mode
  bool modeReading = digitalRead(BUTTON_PIN_2);

  if (modeReading != lastModeBtnState) lastModeDebounceTime = millis();

  if (millis() - lastModeDebounceTime > modeDebounceDelay) {
    if (modeReading != modeBtnState) {
      modeBtnState = modeReading;
      if (modeBtnState == HIGH) {
        int newMode = (fbMode + 1) % 3;
        fbMode = newMode;
        modePending = newMode;
        modeDirty = true;

        SerialLock();
        Serial.println("----");
        Serial.print("Mode pending: ");
        Serial.println(modeTextFull(newMode));
        SerialUnlock();
      }
    }
  }
  lastModeBtnState = modeReading;

  // Button 1: Pump (manual only)
  if (fbMode == 0 && !lowWater) {
    bool reading = digitalRead(BUTTON_PIN_1);

    if (reading != lastButtonState) lastDebounceTime = millis();

    if (millis() - lastDebounceTime > debounceDelay) {
      if (reading != buttonState) {
        buttonState = reading;

        if (buttonState == HIGH) {
          bool newState = !relayState;
          applyRelay(newState);

          fbManualSwitch = newState ? 1 : 0;
          manualSwitchPending = fbManualSwitch;
          manualSwitchDirty = true;

          SerialLock();
          Serial.println("----");
          Serial.print("ManualSwitch pending: ");
          Serial.println(manualSwitchPending);
          SerialUnlock();
        }
      }
    }

    lastButtonState = reading;
  } else {
    bool r = digitalRead(BUTTON_PIN_1);
    lastButtonState = r;
    buttonState = r;
  }
}

// ===== Water =====
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
    lowWater = (volume < 200);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===== Warn LED =====
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

// ===== Soil =====
void soilTask(void *pvParameters) {
  while (1) {
    int raw = readSoilRawAvg();
    int moisturePercent = map(raw, rawDry, rawWet, 0, 100);
    moisturePercent = constrain(moisturePercent, 0, 100);
    soilPercent = moisturePercent;

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===== OLED =====
void displayTask(void *pvParameters) {
  while (1) {
    int sp   = soilPercent;
    int th   = fbThreshold;
    int mode = fbMode;
    int ps   = fbPumpSeconds;
    int vol  = (int)waterVolumeMl;

    int remain = 0;
    if (pumpTimedRunning) {
      uint32_t now = millis();
      if (pumpStopAtMs > now) remain = (int)((pumpStopAtMs - now) / 1000UL);
    }

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
    display.print(vol);
    display.print("ml");

    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("Th:");
    display.print(th);
    display.print(" M:");
    display.print(modeText3(mode));
    display.print(" T:");
    display.print(pumpTimedRunning ? remain : ps);
    display.print("s");

    display.display();
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ===== Firebase =====
void firebaseTask(void *pvParameters) {
  uint32_t lastWrite = 0;
  uint32_t lastRead  = 0;

  while (1) {
    if (Firebase.ready()) {

      if (manualSwitchDirty) {
        int v = manualSwitchPending;
        if (Firebase.setInt(firebaseData, path + "/ManualSwitch", v)) {
          manualSwitchDirty = false;
          fbManualSwitch = v;
        }
      }

      if (modeDirty) {
        int m = modePending;
        if (Firebase.setInt(firebaseData, path + "/Mode", m)) {
          modeDirty = false;
        }
      }

      if (thresholdDirty) {
        int t = thresholdPending;
        if (Firebase.setInt(firebaseData, path + "/Threshold", t)) {
          thresholdDirty = false;
          fbThreshold = t;
        }
      }

      if (pumpSecDirty) {
        int p = pumpSecPending;
        if (Firebase.setInt(firebaseData, path + "/PumpSeconds", p)) {
          pumpSecDirty = false;
          fbPumpSeconds = p;
        }
      }

      if (millis() - lastWrite >= 5000) {
        lastWrite = millis();
        Firebase.setInt(firebaseData, path + "/Soid", (int)soilPercent);
        Firebase.setFloat(firebaseData, path + "/Volume", (float)waterVolumeMl);
      }

      if (millis() - lastRead >= 2000) {
        lastRead = millis();

        if (!modeDirty) {
          if (Firebase.getInt(firebaseData, path + "/Mode")) fbMode = firebaseData.intData();
        }

        if (!thresholdDirty) {
          if (Firebase.getInt(firebaseData, path + "/Threshold")) fbThreshold = firebaseData.intData();
        }

        if (!pumpSecDirty) {
          if (Firebase.getInt(firebaseData, path + "/PumpSeconds")) fbPumpSeconds = firebaseData.intData();
        }

        if (!manualSwitchDirty) {
          if (Firebase.getInt(firebaseData, path + "/ManualSwitch")) fbManualSwitch = firebaseData.intData();
        }

        if (Firebase.getString(firebaseData, path + "/Schedule/Date")) fbSchDate = firebaseData.stringData();
        if (Firebase.getString(firebaseData, path + "/Schedule/Time")) fbSchTime = firebaseData.stringData();
      }
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ===== Control =====
void controlTask(void *pvParameters) {
  while (1) {
    int mode = fbMode;

    if (lowWater) {
      if (pumpTimedRunning) stopTimedPump();
      if (relayState) applyRelay(false);

      if (fbManualSwitch != 0) {
        fbManualSwitch = 0;
        manualSwitchPending = 0;
        manualSwitchDirty = true;
      }

      vTaskDelay(200 / portTICK_PERIOD_MS);
      continue;
    }

    if (pumpTimedRunning && millis() >= pumpStopAtMs) {
      stopTimedPump();
      if (mode == 1) autoNextAllowedMs = millis() + AUTO_COOLDOWN_MS;
    }

    if (mode == 0) {
      if (fbManualSwitch != lastManualSwitchApplied) {
        lastManualSwitchApplied = fbManualSwitch;
        applyRelay(fbManualSwitch == 1);
      }
      if (pumpTimedRunning) stopTimedPump();
      autoNextAllowedMs = 0;
    }
    else if (mode == 1) {
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

// ===== Serial (4 lines like web) =====
void serialTask(void *pvParameters) {
  while (1) {
    int soil = soilPercent;
    int vol  = (int)waterVolumeMl;
    int th   = fbThreshold;
    int mode = fbMode;

    SerialLock();
    Serial.println("----");
    Serial.print("Soid: ");      Serial.println(soil);
    Serial.print("Volume: ");    Serial.println(vol);
    Serial.print("Threshold: "); Serial.println(th);
    Serial.print("Mode: ");      Serial.println(modeTextFull(mode));
    SerialUnlock();

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

// ===== Encoders (2x KY-040) =====
void encoderTask(void *pvParameters) {
  // init AB
  uint8_t thPrev = (digitalRead(ENC_TH_CLK) << 1) | digitalRead(ENC_TH_DT);
  uint8_t psPrev = (digitalRead(ENC_PS_CLK) << 1) | digitalRead(ENC_PS_DT);

  int thAccum = 0;
  int psAccum = 0;

  // SW debounce
  bool thSwLast = HIGH, thSwState = HIGH;
  uint32_t thSwT = 0;

  bool psSwLast = HIGH, psSwState = HIGH;
  uint32_t psSwT = 0;

  const uint32_t swDeb = 50;

  while (1) {
    // --- TH encoder ---
    uint8_t thCurr = (digitalRead(ENC_TH_CLK) << 1) | digitalRead(ENC_TH_DT);
    int8_t thMove = encStepFrom(thPrev, thCurr);
    if (thMove != 0) {
      thAccum += thMove;
      if (thAccum >= 4) {
        thAccum = 0;
        int step = encThStep;
        int newTh = fbThreshold + step;
        if (newTh > 100) newTh = 100;
        if (newTh != fbThreshold) {
          fbThreshold = newTh;
          thresholdPending = newTh;
          thresholdDirty = true;
        }
      } else if (thAccum <= -4) {
        thAccum = 0;
        int step = encThStep;
        int newTh = fbThreshold - step;
        if (newTh < 0) newTh = 0;
        if (newTh != fbThreshold) {
          fbThreshold = newTh;
          thresholdPending = newTh;
          thresholdDirty = true;
        }
      }
    }
    thPrev = thCurr;

    // TH SW: toggle step 1<->5
    bool thSwRead = digitalRead(ENC_TH_SW);
    if (thSwRead != thSwLast) thSwT = millis();
    if (millis() - thSwT > swDeb) {
      if (thSwRead != thSwState) {
        thSwState = thSwRead;
        if (thSwState == LOW) { // press
          encThStep = (encThStep == 1) ? 5 : 1;
        }
      }
    }
    thSwLast = thSwRead;

    // --- PS encoder ---
    uint8_t psCurr = (digitalRead(ENC_PS_CLK) << 1) | digitalRead(ENC_PS_DT);
    int8_t psMove = encStepFrom(psPrev, psCurr);
    if (psMove != 0) {
      psAccum += psMove;
      if (psAccum >= 4) {
        psAccum = 0;
        int step = encPsStep;
        int newPs = fbPumpSeconds + step;
        if (newPs > 999) newPs = 999;
        if (newPs != fbPumpSeconds) {
          fbPumpSeconds = newPs;
          pumpSecPending = newPs;
          pumpSecDirty = true;
        }
      } else if (psAccum <= -4) {
        psAccum = 0;
        int step = encPsStep;
        int newPs = fbPumpSeconds - step;
        if (newPs < 1) newPs = 1;
        if (newPs != fbPumpSeconds) {
          fbPumpSeconds = newPs;
          pumpSecPending = newPs;
          pumpSecDirty = true;
        }
      }
    }
    psPrev = psCurr;

    // PS SW: toggle step 1<->5
    bool psSwRead = digitalRead(ENC_PS_SW);
    if (psSwRead != psSwLast) psSwT = millis();
    if (millis() - psSwT > swDeb) {
      if (psSwRead != psSwState) {
        psSwState = psSwRead;
        if (psSwState == LOW) {
          encPsStep = (encPsStep == 1) ? 5 : 1;
        }
      }
    }
    psSwLast = psSwRead;

    vTaskDelay(2 / portTICK_PERIOD_MS);
  }
}