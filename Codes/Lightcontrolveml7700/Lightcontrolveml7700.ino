#include <Wire.h>
#include <Adafruit_VEML7700.h>

Adafruit_VEML7700 veml;

// ----- PIN -----
const int LED_GATE_PIN = 9;   // Arduino pin -> (100-220Ω) -> MOSFET Gate

// ----- DEFAULT USER SETTINGS -----
unsigned long onTimeMs = 60UL * 1000UL; // default: keep LED ON for 60 seconds
float luxOnThreshold  = 30.0;           // if lux <= this, it's "dark" -> turn ON
float luxOffThreshold = 45.0;           // if lux >= this, it's "bright" -> allow OFF (hysteresis)

// ----- STATE -----
bool ledOn = false;
bool darkLatch = false;
unsigned long ledOnUntilMs = 0;

void setLed(bool on) {
  ledOn = on;
  digitalWrite(LED_GATE_PIN, on ? HIGH : LOW);
}

void startTimedOn(unsigned long nowMs) {
  setLed(true);
  ledOnUntilMs = nowMs + onTimeMs;
}

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  t <seconds>   -> set ON timer duration (e.g., t 120)");
  Serial.println("  th <lux>      -> set ON threshold (dark) (e.g., th 25)");
  Serial.println("  st <lux>      -> set OFF threshold (bright) (e.g., st 40)");
  Serial.println("Notes: Use Serial Monitor 'Newline' line ending.");
  Serial.println();
}

void handleSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  int sp = cmd.indexOf(' ');
  String key = (sp == -1) ? cmd : cmd.substring(0, sp);
  String val = (sp == -1) ? ""  : cmd.substring(sp + 1);

  key.trim(); val.trim();

  if (key == "t") {
    long sec = val.toInt();
    if (sec > 0) {
      onTimeMs = (unsigned long)sec * 1000UL;
      Serial.print("Timer set: "); Serial.print(sec); Serial.println(" seconds");
    } else {
      Serial.println("Usage: t <seconds>");
    }
  } else if (key == "th") {
    float v = val.toFloat();
    if (v >= 0) {
      luxOnThreshold = v;
      Serial.print("luxOnThreshold = "); Serial.println(luxOnThreshold);
    } else {
      Serial.println("Usage: th <lux>");
    }
  } else if (key == "st") {
    float v = val.toFloat();
    if (v >= 0) {
      luxOffThreshold = v;
      Serial.print("luxOffThreshold = "); Serial.println(luxOffThreshold);
    } else {
      Serial.println("Usage: st <lux>");
    }
  } else if (key == "help") {
    printHelp();
  } else {
    Serial.println("Unknown command. Type: help");
  }

  // Sanity: keep thresholds ordered (optional but helpful)
  if (luxOffThreshold < luxOnThreshold) {
    float tmp = luxOffThreshold;
    luxOffThreshold = luxOnThreshold;
    luxOnThreshold = tmp;
    Serial.println("Swapped thresholds so OFF >= ON (hysteresis preserved).");
    Serial.print("luxOnThreshold = "); Serial.println(luxOnThreshold);
    Serial.print("luxOffThreshold = "); Serial.println(luxOffThreshold);
  }
}

void setup() {
  pinMode(LED_GATE_PIN, OUTPUT);
  setLed(false);

  Serial.begin(9600);
  while (!Serial) { delay(10); }

  Wire.begin();

  if (!veml.begin()) {
    Serial.println("ERROR: VEML7700 not found. Check SDA/SCL, power, GND.");
    while (1) { delay(100); }
  }

  // Reasonable defaults; you can tweak later if needed
  veml.setGain(VEML7700_GAIN_1);
  veml.setIntegrationTime(VEML7700_IT_100MS);

  Serial.println("VEML7700 + MOSFET LED controller ready.");
  Serial.print("Default timer (s): "); Serial.println(onTimeMs / 1000UL);
  Serial.print("ON threshold (lux): "); Serial.println(luxOnThreshold);
  Serial.print("OFF threshold (lux): "); Serial.println(luxOffThreshold);
  printHelp();
}

void loop() {
  handleSerial();

  unsigned long nowMs = millis();
  float lux = veml.readLux();

  // Update darkLatch with hysteresis
  if (!darkLatch && lux <= luxOnThreshold) darkLatch = true;
  if (darkLatch && lux >= luxOffThreshold) darkLatch = false;

  // Control logic with timed ON constraint
  if (ledOn) {
    // Keep ON until timer expires
    if ((long)(nowMs - ledOnUntilMs) >= 0) {
      // Timer expired: if still dark, restart another ON window; else turn OFF
      if (darkLatch) startTimedOn(nowMs);
      else setLed(false);
    }
  } else {
    // Currently OFF: if dark, start timed ON
    if (darkLatch) startTimedOn(nowMs);
  }

  // Status print (2 Hz)
  static unsigned long lastPrint = 0;
  if (nowMs - lastPrint >= 500) {
    lastPrint = nowMs;

    Serial.print("Lux: "); Serial.print(lux, 1);
    Serial.print(" | darkLatch: "); Serial.print(darkLatch ? "YES" : "NO");
    Serial.print(" | LED: "); Serial.print(ledOn ? "ON" : "OFF");
    Serial.print(" | ms left: ");
    if (ledOn && (long)(ledOnUntilMs - nowMs) > 0) Serial.println(ledOnUntilMs - nowMs);
    else Serial.println(0);
  }
}
