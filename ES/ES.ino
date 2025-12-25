#define RELAY_PIN   27
#define BUTTON_PIN  32
#define LED_PIN     2

#define TRIG_PIN    5
#define ECHO_PIN    18
#define WARN_LED_PIN 4

#define SOUND_SPEED 0.034
#define CUP_HEIGHT 14.0
#define MAX_H 11.0

bool relayState = true;
bool lastButtonState = LOW;
bool buttonState = LOW;

unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

volatile bool lowWater = false;

void waterTask(void *pvParameters);
void warnLedTask(void *pvParameters);

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(WARN_LED_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(WARN_LED_PIN, LOW);

  xTaskCreate(waterTask, "Water Task", 3000, NULL, 1, NULL);
  xTaskCreate(warnLedTask, "Warn LED Task", 1000, NULL, 1, NULL);
}

void loop() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if (millis() - lastDebounceTime > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == HIGH) {
        relayState = !relayState;
        digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
        digitalWrite(LED_PIN, relayState ? HIGH : LOW);
      }
    }
  }

  lastButtonState = reading;
}

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
    float distanceCm = duration * SOUND_SPEED / 2.0;

    float h = CUP_HEIGHT - distanceCm;
    if (h < 0) h = 0;
    if (h > MAX_H) h = MAX_H;

    float radius = 3.0 + (5.5 - 3.0) * (h / MAX_H);

    float volume = (1.0 / 3.0) * PI * h *
                   (sq(3.0) + 3.0 * radius + sq(radius));

    volume = volume * (800.0 / volumeMax);

    lowWater = (volume < 200);

    Serial.print("Volume (ml): ");
    Serial.println(volume);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

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
