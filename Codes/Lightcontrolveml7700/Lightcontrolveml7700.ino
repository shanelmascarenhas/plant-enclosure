#include <Wire.h>
#include <Adafruit_VEML7700.h>

Adafruit_VEML7700 veml;

const int LED_GATE_PIN = 9;

// ---- User settings ----
unsigned long onTimeMs = 10UL * 1000UL;  // default 10s for testing
float luxOnThreshold  = 200.0;           // dark if <= this  -> turn ON
float luxOffThreshold = 300.0;           // bright if >= this -> turn OFF (hysteresis)

// ---- State machine ----
enum State { LED_OFF, LED_ON_WINDOW };
State state = LED_OFF;

unsigned long windowEndMs = 0;

void forceOffNow() {
  pinMode(LED_GATE_PIN, OUTPUT);
  digitalWrite(LED_GATE_PIN, LOW);
  state = LED_OFF;
}

void ledOnStartWindow(unsigned long nowMs) {
  digitalWrite(LED_GATE_PIN, HIGH);
  state = LED_ON_WINDOW;
  windowEndMs = nowMs + onTimeMs;
}

void ledOff() {
  digitalWrite(LED_GATE_PIN, LOW);
  state = LED_OFF;
}

void printHelp() {
  Serial.println("Commands (Newline line ending):");
  Serial.println("  t <sec>   -> set ON window duration, e.g. t 10");
  Serial.println("  th <lux>  -> set ON threshold (dark), e.g. th 200");
  Serial.println("  st <lux>  -> set OFF threshold (bright), e.g. st 300");
}

void handleSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "help") { printHelp(); return; }

  int sp = cmd.indexOf(' ');
  String key = (sp == -1) ? cmd : cmd.substring(0, sp);
  String val = (sp == -1) ? ""  : cmd.substring(sp + 1);
  key.trim(); val.trim();

  if (key == "t") {
    long sec = val.toInt();
    if (sec > 0) {
      onTimeMs = (unsigned long)sec * 1000UL;
      Serial.print("Timer window = "); Serial.print(sec); Serial.println(" s");
    } else Serial.println("Usage: t <sec>");
  } else if (key == "th") {
    float v = val.toFloat();
    if (v >= 0) {
      luxOnThreshold = v;
      Serial.print("luxOnThreshold = "); Serial.println(luxOnThreshold);
    } else Serial.println("Usage: th <lux>");
  } else if (key == "st") {
    float v = val.toFloat();
    if (v >= 0) {
      luxOffThreshold = v;
      Serial.print("luxOffThreshold = "); Serial.println(luxOffThreshold);
    } else Serial.println("Usage: st <lux>");
  } else {
    Serial.println("Unknown. Type help");
  }

  // Keep hysteresis sane
  if (luxOffThreshold < luxOnThreshold) {
    float tmp = luxOffThreshold;
    luxOffThreshold = luxOnThreshold;
    luxOnThreshold = tmp;
    Serial.println("Swapped thresholds so OFF >= ON.");
  }
}

void setup() {
  // Guarantee OFF at boot
  forceOffNow();

  Serial.begin(9600);
  while (!Serial) delay(10);

  Wire.begin();
  if (!veml.begin()) {
    Serial.println("ERROR: VEML7700 not found.");
    while (1) delay(100);
  }

  veml.setGain(VEML7700_GAIN_1);
  veml.setIntegrationTime(VEML7700_IT_100MS);

  Serial.println("Ready. LED starts OFF.");
  printHelp();
}

void loop() {
  handleSerial();

  unsigned long nowMs = millis();
  float lux = veml.readLux();

  if (state == LED_OFF) {
    // Only turn on if it's dark enough
    if (lux <= luxOnThreshold) {
      ledOnStartWindow(nowMs);
      // Optional: uncomment if you want a message when it turns on
      // Serial.print("ON window started. Lux: "); Serial.println(lux);
    }
  } 
  else if (state == LED_ON_WINDOW) {
    // Do nothing until the timer ends, then decide based on brightness/darkness
    if ((long)(nowMs - windowEndMs) >= 0) {
      // Timer ended: decide next state based on current lux
      if (lux >= luxOffThreshold) {
        ledOff();
        Serial.print("OFF (timer end, bright). Lux: ");
        Serial.println(lux);
      } else if (lux <= luxOnThreshold) {
        // Still dark: start another window
        ledOnStartWindow(nowMs);
        // Optional: uncomment if you want a message when it extends
        // Serial.print("EXTEND (timer end, still dark). Lux: "); Serial.println(lux);
      } else {
        // In between thresholds: hold OFF to prevent chatter
        ledOff();
        Serial.print("OFF (timer end, in-between). Lux: ");
        Serial.println(lux);
      }
    }
  }
}
