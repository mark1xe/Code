/*
  COMBINED: Pump + Button + Soil Moisture + Water Volume + Warn LED + OLED SH1106 + Web Server

  Pins:
    Relay IN     -> GPIO27
    Button       -> GPIO32 (INPUT_PULLUP, button to GND)
    Pump LED     -> GPIO16

    HC-SR04:
      VCC       -> 5V
      GND       -> GND
      TRIG      -> GPIO5
      ECHO      -> GPIO18
      Warn LED  -> GPIO17

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

// --- FreeRTOS mutex for Serial printing ---
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

SemaphoreHandle_t serialMutex = nullptr;

// ===== WiFi/FireBase Define ====
#define WIFI_SSID "minhphuong"
#define WIFI_PASSWORD "01010000"

#define FIREBASE_HOST "https://project2-cd9c9-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "tsnWspwpI5U5nuM1H5FfVbFJgS3ASCravKsg53y9"

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;
String path = "/Var";

// ===== Pump + Button (KEEP ORIGINAL LOGIC) =====
#define RELAY_PIN   27
#define BUTTON_PIN  32
#define LED_PIN     16

volatile bool relayState = false;     // pump OFF at start
bool lastButtonState = HIGH;          // INPUT_PULLUP idle = HIGH
bool buttonState = HIGH;

unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

// ===== Soil Moisture (KEEP ORIGINAL LOGIC) =====
#define SOIL_PIN 34

const int samples = 20;
const int sampleDelayMs = 10;

// Calibration values (keep the same as your current ones)
int rawDry = 2650;
int rawWet = 1150;

volatile int soilRaw = 0;
volatile int soilPercent = 0;

// ===== Water Volume + Warn LED (KEEP ORIGINAL LOGIC) =====
#define TRIG_PIN      5
#define ECHO_PIN      18
#define WARN_LED_PIN  17

#define SOUND_SPEED   0.034
#define CUP_HEIGHT    14.0
#define MAX_H         11.0

volatile bool  lowWater = false;
volatile float waterVolumeMl = 0.0;
volatile float waterDistanceCm = 0.0;

// ===== OLED SH1106 =====
Adafruit_SH1106G display(128, 64, &Wire, -1);

// ===== Task prototypes =====
void waterTask(void *pvParameters);
void warnLedTask(void *pvParameters);
void soilTask(void *pvParameters);
void displayTask(void *pvParameters);
void firebaseTask(void *pvParameters);

// --- Serial mutex helpers ---
static inline void SerialLock() {
  if (serialMutex) xSemaphoreTake(serialMutex, portMAX_DELAY);
}
static inline void SerialUnlock() {
  if (serialMutex) xSemaphoreGive(serialMutex);
}

// ===== Soil helper (KEEP ORIGINAL LOGIC) =====
int readSoilRawAvg() {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(SOIL_PIN);
    delay(sampleDelayMs);  // keep the same sampling delay logic
  }
  return (int)(sum / samples);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Create Serial mutex
  serialMutex = xSemaphoreCreateMutex();

  // --- Pump/Button GPIO ---
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
#endif

  // Initial state (keep the same output logic)
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
#ifdef LED_PIN
  digitalWrite(LED_PIN, relayState ? HIGH : LOW);
#endif

  // --- HC-SR04 + Warn LED ---
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(WARN_LED_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(WARN_LED_PIN, LOW);

  // --- ADC setup (same as your test code) ---
  analogReadResolution(12); // 0..4095
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  // --- OLED setup (keep begin address 0x3C) ---
  Wire.begin(21, 22);
  if (!display.begin(0x3C, true)) {
    SerialLock();
    Serial.println("SH1106 not found at 0x3C. Try 0x3D.");
    SerialUnlock();
    while (1) delay(10);
  }
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("System starting...");
  display.display();

  SerialLock();
  Serial.println("=== Combined System ===");
  Serial.println("Pump logic: toggle on button RELEASE (buttonState == HIGH)");
  Serial.println("Soil sensor: average + map %");
  Serial.println("Volume: lowWater = (volume < 200), warn LED blink 500/500");
  Serial.println("----");
  SerialUnlock();

  // ===== WiFi connect (same style as your DHT example) =====
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    SerialLock();
    Serial.print(".");
    SerialUnlock();
  }
  SerialLock();
  Serial.println();
  Serial.println("WiFi connected.");
  Serial.println("----");
  SerialUnlock();

  // ===== Firebase connect (same style as your DHT example) =====
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // --- Create tasks (same logic; run modules in parallel) ---
  xTaskCreate(waterTask,   "Water Task",    3000, NULL, 1, NULL);
  xTaskCreate(warnLedTask, "Warn LED Task", 1000, NULL, 1, NULL);
  xTaskCreate(soilTask,    "Soil Task",     2500, NULL, 1, NULL);
  xTaskCreate(displayTask, "OLED Task",     2500, NULL, 1, NULL);

  // Firebase upload task (send Soid + Volume every 5s)
  xTaskCreate(firebaseTask, "Firebase Task", 6000, NULL, 1, NULL);
}

void loop() {
  // ===== Pump + Button loop (KEEP ORIGINAL LOGIC 100%) =====
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if (millis() - lastDebounceTime > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;

      // KEEP ORIGINAL LOGIC: toggle when buttonState == HIGH (on release)
      if (buttonState == HIGH) {
        relayState = !relayState;

        digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
#ifdef LED_PIN
        digitalWrite(LED_PIN, relayState ? HIGH : LOW);
#endif

        SerialLock();
        Serial.print("Pump/Relay = ");
        Serial.println(relayState ? "ON" : "OFF");
        Serial.println("----");
        SerialUnlock();
      }
    }
  }

  lastButtonState = reading;
}

// ===== Water Task (KEEP ORIGINAL LOGIC) =====
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

    // Small safety improvement (does not change volume/lowWater logic):
    // If no echo, skip this cycle
    if (duration == 0) {
      SerialLock();
      Serial.println("No echo (timeout).");
      Serial.println("----");
      SerialUnlock();
      vTaskDelay(300 / portTICK_PERIOD_MS);
      continue;
    }

    float distanceCm = duration * SOUND_SPEED / 2.0;
    waterDistanceCm = distanceCm;

    float h = CUP_HEIGHT - distanceCm;
    if (h < 0) h = 0;
    if (h > MAX_H) h = MAX_H;

    float radius = 3.0 + (5.5 - 3.0) * (h / MAX_H);

    float volume = (1.0 / 3.0) * PI * h *
                   (sq(3.0) + 3.0 * radius + sq(radius));

    volume = volume * (800.0 / volumeMax);

    waterVolumeMl = volume;

    // KEEP ORIGINAL LOGIC:
    lowWater = (volume < 200);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===== Warn LED Task (KEEP ORIGINAL LOGIC) =====
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

// ===== Soil Task (KEEP ORIGINAL LOGIC) =====
void soilTask(void *pvParameters) {
  SerialLock();
  Serial.println("Soil moisture task start...");
  Serial.println("Put sensor in AIR to get rawDry, then in WATER to get rawWet.");
  Serial.println("----");
  SerialUnlock();

  while (1) {
    int raw = readSoilRawAvg();
    soilRaw = raw;

    int moisturePercent = map(raw, rawDry, rawWet, 0, 100);
    moisturePercent = constrain(moisturePercent, 0, 100);
    soilPercent = moisturePercent;

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===== OLED Display Task (display only, does not affect logic) =====
void displayTask(void *pvParameters) {
  while (1) {
    // Copy volatile -> local variables for stable display updates
    int   sp   = soilPercent;
    int   sr   = soilRaw;
    float vol  = waterVolumeMl;
    bool  pump = relayState;
    bool  low  = lowWater;

    display.clearDisplay();

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Soil: ");

    display.setTextSize(2);
    display.setCursor(40, 0);
    display.print(sp);
    display.print("%");

    display.setTextSize(1);
    display.setCursor(0, 24);
    display.print("Water: ");

    display.setTextSize(2);
    display.setCursor(50, 24);
    display.print((int)vol);
    display.print("ml");

    display.setTextSize(1);
    display.setCursor(0, 48);
    display.print("Pump: ");
    display.print(pump ? "ON" : "OFF");

    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("Low Water: ");
    display.print(low ? "YES" : "NO");

    display.display();

    vTaskDelay(500 / portTICK_PERIOD_MS); // refresh 2Hz
  }
}

// ===== Firebase Task: upload /Var/Soid and /Var/Volume every 5s =====
void firebaseTask(void *pvParameters) {
  const uint32_t intervalMs = 5000;

  while (1) {
    if (Firebase.ready()) {
      int soidValue = soilPercent;       // /Var/Soid
      float volValue = waterVolumeMl;    // /Var/Volume

      bool ok1 = Firebase.setInt(firebaseData, path + "/Soil", soidValue);
      bool ok2 = Firebase.setFloat(firebaseData, path + "/Volume", volValue);

      SerialLock();
      Serial.println("Firebase upload:");
      Serial.print("Soil: ");
      Serial.println(soidValue);
      Serial.print("Volume: ");
      Serial.println(volValue, 1);

      if (ok1 && ok2) {
        Serial.println("Status: OK");
      } else {
        Serial.print("Status: FAIL - ");
        Serial.println(firebaseData.errorReason());
      }
      Serial.println("----");
      SerialUnlock();
    } else {
      SerialLock();
      Serial.println("Firebase not ready");
      Serial.println("----");
      SerialUnlock();
    }

    vTaskDelay(intervalMs / portTICK_PERIOD_MS);
  }
}
