#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold9pt7b.h> // Required for the font you used

/* 1. TOP DISPLAY (Big Screen - SH1106 I2C) - Uses U8g2 Library
 * -------------------------------------
 * ADDR -> 0x3D
 */

/* 2. BOTTOM DISPLAY (Small Screen - SSD1306 I2C) - Uses Adafruit Library
 * -------------------------------------------
 * ADDR -> 0x3C
 */

#define I2C_SDA 21
#define I2C_SCL 22

#define BOTTOM_ADDR 0x3C
#define TOP_ADDR 0x3D

#define BOTTOM_W 128
#define BOTTOM_H 32

/* 3. ROTARY ENCODER
 * --------------
 * SW   -> GPIO 25
 * DT   -> GPIO 33
 * CLK  -> GPIO 32
 */
#define ENC_CLK 32
#define ENC_DT 33
#define ENC_SW 25

// --- OBJECT INITIALIZATION ---

// Top Display: I2C (SH1106) - Uses U8g2
U8G2_SH1106_128X64_NONAME_F_HW_I2C topDisplay(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// Bottom Display: I2C (SSD1306) - Uses Adafruit SSD1306
// -1 means no reset pin is used
Adafruit_SSD1306 bottomDisplay(BOTTOM_W, BOTTOM_H, &Wire, -1);

// --- GLOBAL VARIABLES ---
float tempLow = 68.0;
float tempHigh = 77.0;
float humLow = 40.0;
float humHigh = 80.0;
int soilLow = 30;
int soilHigh = 70;

// Light / Timer Variables
bool timerEnabled = false;
int timeOnHour = 8;
int timeOnMinute = 0;
int timeOffHour = 20;
int timeOffMinute = 0;
long luxThreshold = 30000;

// Brightness Variable (1-10)
int globalBrightness = 10;

// System Clock
int currentHour = 12;
int currentMinute = 0;
unsigned long lastMinuteTick = 0;

// Edit State Variables
float* pEditVal1 = nullptr;
float* pEditVal2 = nullptr;
float editCurrent = 0;
int editStep = 0;
float currentMinLimit = 0;
float currentMaxLimit = 100;
String editUnit = "";

int tempOnH, tempOnM, tempOffH, tempOffM;
int* pEditHour = nullptr;
int* pEditMin = nullptr;

// Menu Logic
struct MenuItem {
  const char* name;
  void (*action)();
  MenuItem* children;
  uint8_t childCount;
};

enum UIState {
  STATE_MENU,
  STATE_SUBMENU,
  STATE_EDIT_DUAL,
  STATE_EDIT_TIME,
  STATE_EDIT_LUX,
  STATE_EDIT_BRIGHTNESS,
  STATE_SENSOR_TEST
};

UIState uiState = STATE_MENU;

MenuItem* currentMenu = nullptr;
uint8_t currentMenuSize = 0;
int selectedIndex = 0;
int menuScrollOffset = 0;
const int VISIBLE_ITEMS = 4;

MenuItem* menuStack[6];
uint8_t menuSizeStack[6];
int indexStack[6];
int stackDepth = 0;

// --- ENCODER ---
volatile int encoderCount = 0;
int lastEncoderCount = 0;
static const int8_t enc_states[] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
static uint8_t old_AB = 0;

void IRAM_ATTR readEncoder() {
  old_AB <<= 2;
  old_AB |= ((digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT));
  old_AB &= 0x0f;
  if (enc_states[old_AB]) encoderCount += enc_states[old_AB];
}

// --- FORWARD DECLARATIONS ---
void startEditTemp();
void startEditHum();
void startEditSoil();
void toggleTimer();
void startEditLux();
void resetGlobal();
void showPH();
void startSensorTest();
void startSetClock();
void startEditBrightness();

// --- MENU DEFINITIONS ---
MenuItem tempItems[] = { { "Set Range", startEditTemp, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem humItems[] = { { "Set Range", startEditHum, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem soilItems[] = { { "Set Range", startEditSoil, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };

MenuItem lightItems[] = {
  { "Timer: OFF", toggleTimer, nullptr, 0 },
  { "Set LUX Limit", startEditLux, nullptr, 0 },
  { "Back", nullptr, nullptr, 0 }
};

MenuItem settingsItems[] = {
  { "Set Clock", startSetClock, nullptr, 0 },
  { "Brightness", startEditBrightness, nullptr, 0 },
  { "Sensor Test", startSensorTest, nullptr, 0 },
  { "Global Reset", resetGlobal, nullptr, 0 },
  { "Back", nullptr, nullptr, 0 }
};

MenuItem mainMenu[] = {
  { "Temperature", nullptr, tempItems, 2 },
  { "Humidity", nullptr, humItems, 2 },
  { "Soil Moisture", nullptr, soilItems, 2 },
  { "Light Control", nullptr, lightItems, 3 },
  { "pH Level", showPH, nullptr, 0 },
  { "Settings", nullptr, settingsItems, 5 }
};

// --- HELPER FUNCTIONS ---

void applyBrightness(int level) {
  int contrast = map(level, 1, 10, 1, 255);
  
  // Set U8g2 Contrast
  topDisplay.setContrast(contrast);
  
  // Set Adafruit SSD1306 Contrast
  // Adafruit library handles dimming differently, but we can send raw command
  bottomDisplay.ssd1306_command(SSD1306_SETCONTRAST);
  bottomDisplay.ssd1306_command(contrast);
}

String getLuxPlantType(long lux) {
  if (lux < 2500) return "Low: Pothos/Snake";
  if (lux < 10000) return "Med: Ferns/Peace";
  if (lux < 20000) return "High: Monstera";
  if (lux < 50000) return "Direct: Herbs";
  return "Full Sun: Crops";
}

void updateClock() {
  if (millis() - lastMinuteTick >= 60000) {
    lastMinuteTick = millis();
    currentMinute++;
    if (currentMinute > 59) {
      currentMinute = 0;
      currentHour++;
      if (currentHour > 23) currentHour = 0;
    }
  }
}

String getCountdownStr() {
  if (!timerEnabled) return "Disabled";

  int nowMins = (currentHour * 60) + currentMinute;
  int onMins = (timeOnHour * 60) + timeOnMinute;
  int offMins = (timeOffHour * 60) + timeOffMinute;

  bool isLightOn = false;
  if (timeOnHour < timeOffHour) {
    if (nowMins >= onMins && nowMins < offMins) isLightOn = true;
  } else {
    if (nowMins >= onMins || nowMins < offMins) isLightOn = true;
  }

  int targetMins = isLightOn ? offMins : onMins;
  int diff = targetMins - nowMins;
  if (diff < 0) diff += 1440;

  int h = diff / 60;
  int m = diff % 60;

  String prefix = isLightOn ? "Off: " : "On: ";
  String sH = String(h);
  String sM = (m < 10) ? "0" + String(m) : String(m);

  return prefix + sH + "h " + sM + "m";
}

// --- DRAWING FUNCTIONS ---

void updateBottomMenu(String line1, String line2 = "") {
  bottomDisplay.clearDisplay();
  bottomDisplay.setTextColor(SSD1306_WHITE);
  
  // Line 1: Standard font
  bottomDisplay.setFont(NULL); 
  bottomDisplay.setTextSize(1);
  bottomDisplay.setCursor(0, 0);
  bottomDisplay.print(line1);

  // Line 2: Big Bold font
  if (line2 != "") {
    bottomDisplay.setFont(&FreeSansBold9pt7b);
    bottomDisplay.setCursor(0, 28);
    bottomDisplay.print(line2);
  }
  bottomDisplay.display();
}

void showHoverContext(const char* itemName) {
  String name = String(itemName);
  String valLine = "";

  if (name == "Temperature") {
    valLine = "L:" + String((int)tempLow) + (char)247 + " H:" + String((int)tempHigh) + (char)247;
  } else if (name == "Humidity") {
    valLine = "L:" + String((int)humLow) + "% H:" + String((int)humHigh) + "%";
  } else if (name == "Soil Moist") {
    valLine = "L:" + String(soilLow) + "% H:" + String(soilHigh) + "%";
  } else if (name == "Light Control") { // Fixed name match
    valLine = getCountdownStr();
  } else if (name == "pH Level") {
    valLine = "Current: 7.0";
  } else if (name == "Settings") {
    valLine = "System Setup";
  } else if (String(itemName).startsWith("Timer")) {
    valLine = timerEnabled ? "Status: ON" : "Status: OFF";
  } else if (name == "Set Clock") {
    String m = (currentMinute < 10) ? "0" + String(currentMinute) : String(currentMinute);
    valLine = "Time: " + String(currentHour) + ":" + m;
  } else if (name == "Brightness") {
    valLine = "Level: " + String(globalBrightness) + "/10";
  } else {
    valLine = "Select to Edit";
  }
  
  bottomDisplay.clearDisplay();
  bottomDisplay.setTextColor(SSD1306_WHITE);
  
  bottomDisplay.setFont(NULL);
  bottomDisplay.setTextSize(1);
  bottomDisplay.setCursor(0, 0);
  bottomDisplay.print(name);

  bottomDisplay.setFont(&FreeSansBold9pt7b);
  bottomDisplay.setCursor(0, 28);
  bottomDisplay.print(valLine);
  
  bottomDisplay.display();
}

void drawTimeEdit(int h, int m, bool editingHour, String title) {
  // TOP DISPLAY (U8G2)
  topDisplay.firstPage();
  do {
    topDisplay.setFont(u8g2_font_ncenB10_tr);
    int wTitle = topDisplay.getStrWidth(title.c_str());
    topDisplay.setCursor(64 - (wTitle / 2), 12);
    topDisplay.print(title);
    topDisplay.setFont(u8g2_font_logisoso24_tr);
    if (editingHour) topDisplay.drawFrame(18, 22, 42, 32);
    topDisplay.setCursor(20, 50);
    if (h < 10) topDisplay.print("0");
    topDisplay.print(h);
    topDisplay.setCursor(63, 47);
    topDisplay.print(":");
    if (!editingHour) topDisplay.drawFrame(73, 22, 42, 32);
    topDisplay.setCursor(75, 50);
    if (m < 10) topDisplay.print("0");
    topDisplay.print(m);
    topDisplay.setFont(u8g2_font_6x10_tf);
    if (editingHour) topDisplay.drawStr(39, 62, "^");
    else topDisplay.drawStr(94, 62, "^");
  } while (topDisplay.nextPage());

  // BOTTOM DISPLAY (ADAFRUIT)
  bottomDisplay.clearDisplay();
  bottomDisplay.setTextColor(SSD1306_WHITE);

  // Title (Small)
  bottomDisplay.setFont(NULL);
  bottomDisplay.setTextSize(1);
  bottomDisplay.setCursor(0, 0);
  bottomDisplay.print(title);

  // Value (Big Bold)
  bottomDisplay.setFont(&FreeSansBold9pt7b);
  bottomDisplay.setCursor(20, 29);
  String sH = (h < 10) ? "0" + String(h) : String(h);
  String sM = (m < 10) ? "0" + String(m) : String(m);
  bottomDisplay.print(sH + ":" + sM);
  bottomDisplay.display();
}

void drawLuxEdit(long currentLux) {
  String info = getLuxPlantType(currentLux);

  //TOP DISPLAY (Bar Graph + Plant Info)
  topDisplay.firstPage();
  do {
    topDisplay.setFont(u8g2_font_helvB10_tr);

    int wInfo = topDisplay.getStrWidth(info.c_str());
    topDisplay.setCursor(64 - (wInfo / 2), 20);
    topDisplay.print(info);

    topDisplay.drawFrame(10, 30, 108, 12);
    int w = map(currentLux, 0, 120000, 0, 106);
    if (w > 106) w = 106;
    topDisplay.drawBox(11, 31, w, 10);

    topDisplay.setFont(u8g2_font_6x10_tf);
    topDisplay.setCursor(10, 55);
    topDisplay.print(currentLux);
    topDisplay.print(" lux");
  } while (topDisplay.nextPage());

  // --- BOTTOM DISPLAY (Context + Large Value) ---
  bottomDisplay.clearDisplay();
  bottomDisplay.setTextColor(SSD1306_WHITE);

  // Line 1: Explanation
  bottomDisplay.setFont(NULL);  // Standard small font
  bottomDisplay.setTextSize(1);
  bottomDisplay.setCursor(0, 0);
  bottomDisplay.print("Light turns ON below:");

  // Line 2: Value (Using the Adafruit FreeSans font here is fine)
  bottomDisplay.setFont(&FreeSansBold9pt7b);
  bottomDisplay.setCursor(10, 29);
  bottomDisplay.print(currentLux);

  // Unit
  bottomDisplay.setFont(NULL);
  bottomDisplay.setCursor(80, 20);
  bottomDisplay.print("lux");
  bottomDisplay.display();
}

void drawBrightnessEdit(int level) {
  topDisplay.firstPage();
  do {
    topDisplay.setFont(u8g2_font_ncenB10_tr);
    topDisplay.drawStr(20, 15, "BRIGHTNESS");
    topDisplay.drawFrame(10, 25, 108, 14);
    int w = map(level, 1, 10, 5, 106);
    topDisplay.drawBox(11, 26, w, 12);
    topDisplay.setFont(u8g2_font_6x10_tf);
    topDisplay.setCursor(60, 55);
    topDisplay.print(level);
    topDisplay.print("/10");
  } while (topDisplay.nextPage());

  bottomDisplay.clearDisplay();
  bottomDisplay.setTextColor(SSD1306_WHITE);

  // Title (Small)
  bottomDisplay.setFont(NULL);
  bottomDisplay.setTextSize(1);
  bottomDisplay.setCursor(0, 0);
  bottomDisplay.print("ADJUST BRIGHTNESS");

  // Value (Big Bold)
  bottomDisplay.setFont(&FreeSansBold9pt7b);
  bottomDisplay.setCursor(40, 29);
  bottomDisplay.print(level);
  bottomDisplay.display();
}

void drawSensorTest() {
  topDisplay.firstPage();
  do {
    topDisplay.setFont(u8g2_font_ncenB14_tr);
    topDisplay.drawStr(10, 30, "Sensor Test");
    topDisplay.setFont(u8g2_font_6x10_tf);
    topDisplay.drawStr(20, 50, "(Mock Mode)");
  } while (topDisplay.nextPage());

  bottomDisplay.clearDisplay();
  bottomDisplay.setTextColor(SSD1306_WHITE);
  bottomDisplay.setFont(NULL);
  bottomDisplay.setTextSize(1);
  bottomDisplay.setCursor(0, 0);
  bottomDisplay.print("READING...");
  bottomDisplay.setCursor(0, 10);
  bottomDisplay.print("T: 72F  H: 45%");
  bottomDisplay.setCursor(0, 20);
  bottomDisplay.print("L: 5400  S: 40%");
  bottomDisplay.display();
}

//circle

void drawCircularGauge(float valLower, float valUpper, float minLimit, float maxLimit, bool editingUpper) {
  int cx = 64;
  int cy = 32;
  int r_outer = 30;
  int r_inner = 22;

  // Brush parameters
  int r_mid = (r_outer + r_inner) / 2;    // Path Center
  int r_brush = (r_outer - r_inner) / 2;  // Thickness Radius

  float range = maxLimit - minLimit;
  if (range <= 0) range = 1;

  float angleStart = ((valLower - minLimit) / range) * 6.28 - 1.57;
  float angleEnd = ((valUpper - minLimit) / range) * 6.28 - 1.57;

  float a1 = min(angleStart, angleEnd);
  float a2 = max(angleStart, angleEnd);

  // Trace path with solid brush
  float step = 0.04;
  for (float a = a1; a <= a2; a += step) {
    topDisplay.drawDisc(cx + r_mid * cos(a), cy + r_mid * sin(a), r_brush);
  }
  // Ensure final cap is drawn
  topDisplay.drawDisc(cx + r_mid * cos(a2), cy + r_mid * sin(a2), r_brush);

  // Labels
  topDisplay.setFont(u8g2_font_4x6_tf);
  int wLabel = topDisplay.getStrWidth(editingUpper ? "UPPER" : "LOWER");
  topDisplay.setCursor(cx - (wLabel / 2), cy - 4);
  topDisplay.print(editingUpper ? "UPPER" : "LOWER");

  topDisplay.setFont(u8g2_font_6x10_tf);
  int val = editingUpper ? (int)valUpper : (int)valLower;
  String valStr = String(val);
  valStr += (editUnit == "F") ? "\xb0" : "%";
  int wVal = topDisplay.getStrWidth(valStr.c_str());
  topDisplay.setCursor(cx - (wVal / 2), cy + 6);
  topDisplay.print(valStr);
}

void updateBottomEdit(String label, float currentVal, String refLabel, float refVal) {
  bottomDisplay.clearDisplay();
  bottomDisplay.setTextColor(SSD1306_WHITE);

  // Label (Small)
  bottomDisplay.setFont(NULL);
  bottomDisplay.setTextSize(1);
  bottomDisplay.setCursor(0, 0);
  bottomDisplay.print(label);

  // Ref Value (Small, Right Aligned)
  String refText = refLabel + String((int)refVal);
  if (editUnit == "F") refText += (char)247;
  else refText += "%";
  
  // Simple layout calculation for small font
  int wRef = refText.length() * 6;  // Approx width
  bottomDisplay.setCursor(128 - wRef, 0);
  bottomDisplay.print(refText);

  // Main Value (Big Bold)
  bottomDisplay.setFont(&FreeSansBold9pt7b);
  String valText = String((int)currentVal);
  if (editUnit == "F") valText += (char)247;
  else valText += "%";

  int16_t x1, y1;
  uint16_t w, h;
  bottomDisplay.getTextBounds(valText, 0, 0, &x1, &y1, &w, &h);
  bottomDisplay.setCursor((128 - w) / 2, 29);  // Center
  bottomDisplay.print(valText);

  bottomDisplay.display();
}

// --- ACTIONS ---
void startEditTemp() {
  uiState = STATE_EDIT_DUAL;
  editStep = 0;
  pEditVal1 = &tempLow;
  pEditVal2 = &tempHigh;
  currentMinLimit = 20.0;
  currentMaxLimit = 100.0;
  editUnit = "F";
  editCurrent = *pEditVal1;
  encoderCount = (int)editCurrent * 4;
  lastEncoderCount = (int)editCurrent;
}

void startEditHum() {
  uiState = STATE_EDIT_DUAL;
  editStep = 0;
  pEditVal1 = &humLow;
  pEditVal2 = &humHigh;
  currentMinLimit = 0.0;
  currentMaxLimit = 100.0;
  editUnit = "%";
  editCurrent = *pEditVal1;
  encoderCount = (int)editCurrent * 4;
  lastEncoderCount = (int)editCurrent;
}
void startEditSoil() {
  startEditHum();
}

void startEditSchedule() {
  uiState = STATE_EDIT_TIME;
  editStep = 0;
  tempOnH = timeOnHour;
  tempOnM = timeOnMinute;
  tempOffH = timeOffHour;
  tempOffM = timeOffMinute;
  editCurrent = tempOnH;
  encoderCount = (int)editCurrent * 4;
  lastEncoderCount = (int)editCurrent;
}

void toggleTimer() {
  if (!timerEnabled) {
    timerEnabled = true;
    startEditSchedule();
  } else {
    timerEnabled = false;
    updateBottomMenu("Timer Disabled");
    delay(800);
  }
}

void startSetClock() {
  uiState = STATE_EDIT_TIME;
  editStep = 10;
  editCurrent = currentHour;
  encoderCount = (int)editCurrent * 4;
  lastEncoderCount = (int)editCurrent;
}

void startEditLux() {
  uiState = STATE_EDIT_LUX;
  editCurrent = luxThreshold;
  encoderCount = (luxThreshold / 100) * 4;
  lastEncoderCount = encoderCount / 4;
}

void startEditBrightness() {
  uiState = STATE_EDIT_BRIGHTNESS;
  editCurrent = globalBrightness;
  encoderCount = globalBrightness * 4;
  lastEncoderCount = globalBrightness;
}

void resetGlobal() {
  tempLow = 68.0;
  tempHigh = 77.0;
  humLow = 50.0;
  humHigh = 60.0;
  soilLow = 40;
  soilHigh = 60;
  timeOnHour = 8;
  timeOnMinute = 0;
  timeOffHour = 20;
  timeOffMinute = 0;
  luxThreshold = 50000;
  timerEnabled = false;

  globalBrightness = 10;
  applyBrightness(globalBrightness);

  updateBottomMenu("Defaults", "Restored");
  delay(1500);
}
void showPH() {
  updateBottomMenu("pH: 7.0", "Sensor OK");
  delay(1000);
}
void startSensorTest() {
  uiState = STATE_SENSOR_TEST;
}

void goBack() {
  if (stackDepth == 0) {
    currentMenu = mainMenu;
    currentMenuSize = 6;
    selectedIndex = 0;
    uiState = STATE_MENU;
  } else {
    stackDepth--;
    currentMenu = menuStack[stackDepth];
    currentMenuSize = menuSizeStack[stackDepth];
    selectedIndex = indexStack[stackDepth];
    uiState = STATE_SUBMENU;
    if (stackDepth == 0) uiState = STATE_MENU;
  }
  menuScrollOffset = 0;
}

void enterSubMenu(MenuItem* item) {
  menuStack[stackDepth] = currentMenu;
  menuSizeStack[stackDepth] = currentMenuSize;
  indexStack[stackDepth] = selectedIndex;
  stackDepth++;
  currentMenu = item->children;
  currentMenuSize = item->childCount;
  selectedIndex = 0;
  menuScrollOffset = 0;
  uiState = STATE_SUBMENU;
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  
  // 1. Initialize Wire FIRST
  Wire.begin(I2C_SDA, I2C_SCL);

  // 2. Initialize Encoders
  pinMode(ENC_CLK, INPUT);
  pinMode(ENC_DT, INPUT);
  pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT), readEncoder, CHANGE);

  // 3. Initialize BOTTOM Display (Adafruit - Address 0x3C)
  // Note: Adafruit library automatically uses 0x3C if not specified, 
  // but we pass it just in case.
  if(!bottomDisplay.begin(SSD1306_SWITCHCAPVCC, BOTTOM_ADDR)) {
    Serial.println(F("SSD1306 allocation failed"));
    // Don't freeze loop, just continue, maybe top display works
  }
  bottomDisplay.display(); 
  delay(100);
  bottomDisplay.clearDisplay();

  // 4. Initialize TOP Display (U8g2 - Address 0x3D)
  // U8g2 expects address * 2
  topDisplay.setI2CAddress(TOP_ADDR * 2);
  topDisplay.begin();
  topDisplay.setFont(u8g2_font_6x10_tf);

  // 5. Initial Draw
  applyBrightness(globalBrightness);

  currentMenu = mainMenu;
  currentMenuSize = 6;
  topDisplay.clearBuffer();
  topDisplay.drawStr(30, 30, "READY");
  topDisplay.sendBuffer();
  
  updateBottomMenu("Welcome");
  delay(1000);
  lastMinuteTick = millis();
}

// --- LOOP ---
void loop() {
  updateClock();

  int currentEnc = encoderCount / 4;
  int diff = lastEncoderCount - currentEnc;

  if (diff != 0) {
    lastEncoderCount = currentEnc;

    if (uiState == STATE_MENU || uiState == STATE_SUBMENU) {
      selectedIndex += diff;
      if (selectedIndex < 0) selectedIndex = currentMenuSize - 1;
      if (selectedIndex >= currentMenuSize) selectedIndex = 0;
      if (selectedIndex < menuScrollOffset) menuScrollOffset = selectedIndex;
      if (selectedIndex >= menuScrollOffset + VISIBLE_ITEMS) menuScrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
    } else if (uiState == STATE_EDIT_DUAL) {
      editCurrent += diff;
      if (editStep == 0) {
        if (editCurrent < currentMinLimit) editCurrent = currentMinLimit;
        if (editCurrent > currentMaxLimit) editCurrent = currentMaxLimit;
        if (editCurrent > *pEditVal2) *pEditVal2 = editCurrent;
      } else {
        if (editCurrent < *pEditVal1) editCurrent = *pEditVal1;
        if (editCurrent > currentMaxLimit) editCurrent = currentMaxLimit;
      }
    } else if (uiState == STATE_EDIT_TIME) {
      editCurrent += diff;
      int limit = (editStep == 0 || editStep == 2 || editStep == 10) ? 23 : 59;
      if (editCurrent < 0) editCurrent = limit;
      if (editCurrent > limit) editCurrent = 0;
    } else if (uiState == STATE_EDIT_LUX) {
      long step = 100;
      if (abs(diff) > 1) step = 500;
      editCurrent += (diff * step);
      if (editCurrent < 0) editCurrent = 0;
      if (editCurrent > 120000) editCurrent = 120000;
    } else if (uiState == STATE_EDIT_BRIGHTNESS) {
      editCurrent += diff;
      if (editCurrent < 1) editCurrent = 1;
      if (editCurrent > 10) editCurrent = 10;
      globalBrightness = (int)editCurrent;
      applyBrightness(globalBrightness);
    }
  }

  static unsigned long lastBtnTime = 0;
  bool clicked = false;
  if (digitalRead(ENC_SW) == LOW) {
    if (millis() - lastBtnTime > 250) {
      clicked = true;
      lastBtnTime = millis();
    }
  }

  if (uiState == STATE_MENU || uiState == STATE_SUBMENU) {
    topDisplay.firstPage();
    do {
      topDisplay.setFont(u8g2_font_6x10_tf);
      topDisplay.drawStr(0, 10, (uiState == STATE_MENU) ? "-- MAIN MENU--" : "-- SUB --");
      for (int i = 0; i < currentMenuSize; i++) {
        if (i >= menuScrollOffset && i < menuScrollOffset + VISIBLE_ITEMS) {
          int y = 25 + ((i - menuScrollOffset) * 12);
          if (i == selectedIndex) {
            topDisplay.setFont(u8g2_font_7x14B_tf);
            if (String(currentMenu[i].name).startsWith("Timer")) {
              String label = timerEnabled ? "Timer: ON" : "Timer: OFF";
              topDisplay.drawStr(0, y, ">");
              topDisplay.drawStr(10, y, label.c_str());
            } else {
              topDisplay.drawStr(0, y, ">");
              topDisplay.drawStr(10, y, currentMenu[i].name);
            }
          } else {
            topDisplay.setFont(u8g2_font_6x10_tf);
            if (String(currentMenu[i].name).startsWith("Timer")) {
              String label = timerEnabled ? "Timer: ON" : "Timer: OFF";
              topDisplay.drawStr(10, y, label.c_str());
            } else {
              topDisplay.drawStr(10, y, currentMenu[i].name);
            }
          }
        }
      }
    } while (topDisplay.nextPage());

    static int lastIdx = -1;
    if (selectedIndex != lastIdx) {
      showHoverContext(currentMenu[selectedIndex].name);
      lastIdx = selectedIndex;
    }

    if (clicked) {
      MenuItem& item = currentMenu[selectedIndex];
      if (String(item.name) == "Back") goBack();
      else if (item.children != nullptr) enterSubMenu(&item);
      else if (item.action != nullptr) item.action();
    }
  }

  else if (uiState == STATE_EDIT_DUAL) {
    int valL = (editStep == 0) ? (int)editCurrent : (int)*pEditVal1;
    int valH = (editStep == 1) ? (int)editCurrent : (int)*pEditVal2;
    topDisplay.firstPage();
    do { drawCircularGauge(valL, valH, currentMinLimit, currentMaxLimit, (editStep == 1)); } while (topDisplay.nextPage());

    static float lastValDisp = -999;
    static int lastStepDisp = -1;
    if (lastValDisp != editCurrent || lastStepDisp != editStep || (editStep == 0 && *pEditVal2 == editCurrent)) {
      if (editStep == 0) updateBottomEdit("SET LOWER", editCurrent, "High: ", *pEditVal2);
      else updateBottomEdit("SET UPPER", editCurrent, "Low: ", *pEditVal1);
      lastValDisp = editCurrent;
      lastStepDisp = editStep;
    }
    if (clicked) {
      if (editStep == 0) {
        *pEditVal1 = editCurrent;
        editStep = 1;
        editCurrent = *pEditVal2;
        encoderCount = (int)editCurrent * 4;
        lastEncoderCount = (int)editCurrent;
      } else {
        *pEditVal2 = editCurrent;
        updateBottomMenu("RANGE", "SAVED");
        delay(800);
        goBack();
      }
    }
  }

  else if (uiState == STATE_EDIT_TIME) {
    if (editStep < 10) {
      String title = (editStep < 2) ? "TURN ON TIME" : "TURN OFF TIME";
      bool isHour = (editStep == 0 || editStep == 2);
      int showH = (editStep == 0) ? (int)editCurrent : (editStep == 1) ? tempOnH
                                                                       : (editStep == 2) ? (int)editCurrent
                                                                                         : tempOffH;
      int showM = (editStep == 0) ? tempOnM : (editStep == 1) ? (int)editCurrent
                                                              : (editStep == 2) ? tempOffM
                                                                                : (int)editCurrent;
      drawTimeEdit(showH, showM, isHour, title);

      if (clicked) {
        if (editStep == 0) {
          tempOnH = (int)editCurrent;
          editStep = 1;
          editCurrent = tempOnM;
        } else if (editStep == 1) {
          tempOnM = (int)editCurrent;
          editStep = 2;
          editCurrent = tempOffH;
        } else if (editStep == 2) {
          tempOffH = (int)editCurrent;
          editStep = 3;
          editCurrent = tempOffM;
        } else {
          tempOffM = (int)editCurrent;
          timeOnHour = tempOnH;
          timeOnMinute = tempOnM;
          timeOffHour = tempOffH;
          timeOffMinute = tempOffM;
          updateBottomMenu("SCHEDULE", "SAVED");
          delay(800);
          goBack();
        }
        encoderCount = (int)editCurrent * 4;
        lastEncoderCount = (int)editCurrent;
        delay(200);
      }
    } else {
      String title = "SET CLOCK";
      bool isHour = (editStep == 10);
      int showH = (editStep == 10) ? (int)editCurrent : currentHour;
      int showM = (editStep == 11) ? (int)editCurrent : currentMinute;
      drawTimeEdit(showH, showM, isHour, title);

      if (clicked) {
        if (editStep == 10) {
          currentHour = (int)editCurrent;
          editStep = 11;
          editCurrent = currentMinute;
        } else {
          currentMinute = (int)editCurrent;
          updateBottomMenu("CLOCK", "UPDATED");
          delay(800);
          goBack();
        }
        encoderCount = (int)editCurrent * 4;
        lastEncoderCount = (int)editCurrent;
        delay(200);
      }
    }
  }

  else if (uiState == STATE_EDIT_LUX) {
    drawLuxEdit((long)editCurrent);
    if (clicked) {
      luxThreshold = (long)editCurrent;
      updateBottomMenu("LUX LIMIT", "SAVED");
      delay(800);
      goBack();
    }
  } else if (uiState == STATE_EDIT_BRIGHTNESS) {
    drawBrightnessEdit((int)editCurrent);
    if (clicked) {
      updateBottomMenu("BRIGHTNESS", "SAVED");
      delay(800);
      goBack();
    }
  } else if (uiState == STATE_SENSOR_TEST) {
    drawSensorTest();
    if (clicked) goBack();
  }
}