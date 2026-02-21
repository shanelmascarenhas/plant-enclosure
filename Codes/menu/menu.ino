#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

/* ==========================================
 * PIN DEFINITIONS
 * ========================================== */
// I2C Pins
#define I2C_SDA 21
#define I2C_SCL 22

// I2C Addresses (7-bit)
#define BOTTOM_ADDR 0x3C
#define TOP_ADDR    0x3D

/* ROTARY ENCODER
 * SW   -> GPIO 25
 * DT   -> GPIO 33
 * CLK  -> GPIO 32
 */
#define ENC_CLK 32
#define ENC_DT 33
#define ENC_SW 25

/* ==========================================
 * OBJECT INITIALIZATION
 * ========================================== */
U8G2_SH1106_128X64_NONAME_F_HW_I2C topDisplay(U8G2_R0, /* clock=*/22, /* data=*/21, /* reset=*/U8X8_PIN_NONE);
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C bottomDisplay(U8G2_R0, /* clock=*/22, /* data=*/21, /* reset=*/U8X8_PIN_NONE);

/* ==========================================
 * GLOBAL VARIABLES
 * ========================================== */
WebServer server(80);

// Sensor Thresholds & Settings
float tempLow = 68.0, tempHigh = 77.0, humLow = 40.0, humHigh = 80.0;
int soilLow = 30, soilHigh = 70, globalBrightness = 10;
bool timerEnabled = false;
int timeOnHour = 8, timeOnMinute = 0, timeOffHour = 20, timeOffMinute = 0;
long luxThreshold = 30000;

// System Clock
int currentHour = 12, currentMinute = 0;
unsigned long lastMinuteTick = 0;

// Edit State Helpers
float *pEditVal1 = nullptr, *pEditVal2 = nullptr, editCurrent = 0;
int editStep = 0;
float currentMinLimit = 0, currentMaxLimit = 100;
String editUnit = "", currentHeaderName = "-- MAIN MENU --";
int tempOnH, tempOnM, tempOffH, tempOffM;

// --- FIREBASE URL ---
const char* firebaseURL = "https://plant-enclosure-default-rtdb.firebaseio.com/settings.json";

// Menu Logic Structures
struct MenuItem {
  const char* name;
  void (*action)();
  MenuItem* children;
  uint8_t childCount;
};

enum UIState { STATE_MENU, STATE_SUBMENU, STATE_EDIT_DUAL, STATE_EDIT_TIME, STATE_EDIT_LUX, STATE_EDIT_BRIGHTNESS, STATE_SENSOR_TEST };
UIState uiState = STATE_MENU;
MenuItem *currentMenu = nullptr, *menuStack[6];
uint8_t currentMenuSize = 0, menuSizeStack[6], stackDepth = 0;
int selectedIndex = 0, menuScrollOffset = 0, indexStack[6];
const int VISIBLE_ITEMS = 4;

// Encoder Variables
volatile int encoderCount = 0;
int lastEncoderCount = 0;
static const int8_t enc_states[] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
static uint8_t old_AB = 0;

/* ==========================================
 * INTERRUPT ROUTINES
 * ========================================== */
void IRAM_ATTR readEncoder() {
  old_AB <<= 2;
  old_AB |= ((digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT));
  old_AB &= 0x0f;
  if (enc_states[old_AB]) encoderCount += enc_states[old_AB];
}

/* ==========================================
 * FORWARD DECLARATIONS
 * ========================================== */
void startEditTemp(); void startEditHum(); void startEditSoil(); void toggleTimer();
void startEditLux(); void resetGlobal(); void showPH(); void startSensorTest();
void startSetClock(); void startEditBrightness(); void goBack();
void startWiFiSetup(); void showWiFiIP(); void resetWiFi();
void setupServer(); void handleSettingsPost(); void handleOptions();

/* ==========================================
 * MENU DEFINITIONS
 * ========================================== */
MenuItem tempItems[] = { { "Set Range", startEditTemp, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem humItems[] = { { "Set Range", startEditHum, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem soilItems[] = { { "Set Range", startEditSoil, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem lightItems[] = { { "Timer: OFF", toggleTimer, nullptr, 0 }, { "Set LUX Limit", startEditLux, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem wifiItems[] = { { "Setup", startWiFiSetup, nullptr, 0 }, { "Show IP", showWiFiIP, nullptr, 0 }, { "Reset WiFi", resetWiFi, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem settingsItems[] = { { "WiFi", nullptr, wifiItems, 4 }, { "Set Clock", startSetClock, nullptr, 0 }, { "Brightness", startEditBrightness, nullptr, 0 }, { "Sensor Test", startSensorTest, nullptr, 0 }, { "Global Reset", resetGlobal, nullptr, 0 }, { "Back", nullptr, nullptr, 0 } };
MenuItem mainMenu[] = { { "Temperature", nullptr, tempItems, 2 }, { "Humidity", nullptr, humItems, 2 }, { "Soil Moisture", nullptr, soilItems, 2 }, { "Light Control", nullptr, lightItems, 3 }, { "pH Level", showPH, nullptr, 0 }, { "Settings", nullptr, settingsItems, 6 } };

/* ==========================================
 * HELPER FUNCTIONS
 * ========================================== */
void applyBrightness(int level) {
  int contrast = map(level, 1, 10, 50, 255);
  topDisplay.setContrast(contrast);
  bottomDisplay.setContrast(contrast);
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

/* ==========================================
 * U8G2 DRAWING FUNCTIONS
 * ========================================== */
int getCenterX(U8G2& display, String text) { return (display.getDisplayWidth() - display.getStrWidth(text.c_str())) / 2; }

void updateBottomMenu(String line1, String line2 = "") {
  bottomDisplay.clearBuffer();
  bottomDisplay.setFont(u8g2_font_6x10_tf);
  bottomDisplay.setCursor(0, 10);
  bottomDisplay.print(line1);
  if (line2 != "") {
    bottomDisplay.setFont(u8g2_font_helvB12_tr);
    bottomDisplay.setCursor(0, 30);
    bottomDisplay.print(line2);
  }
  bottomDisplay.sendBuffer();
}

void showHoverContext(const char* itemName) {
  String name = String(itemName);
  String valLine = "";
  if (name == "Temperature") valLine = "L:" + String((int)tempLow) + "\xb0 H:" + String((int)tempHigh) + "\xb0";
  else if (name == "Humidity") valLine = "L:" + String((int)humLow) + "% H:" + String((int)humHigh) + "%";
  else if (name == "Soil Moisture") valLine = "L:" + String(soilLow) + "% H:" + String(soilHigh) + "%";
  else if (name == "Light Control") valLine = getCountdownStr();
  else if (name == "pH Level") valLine = "Current: 7.0";
  else if (name == "Settings") valLine = "System Setup";
  else if (String(itemName).startsWith("Timer")) valLine = timerEnabled ? "Status: ON" : "Status: OFF";
  else if (name == "Set Clock") { String m = (currentMinute < 10) ? "0" + String(currentMinute) : String(currentMinute); valLine = "Time: " + String(currentHour) + ":" + m; }
  else if (name == "Brightness") valLine = "Level: " + String(globalBrightness) + "/10";
  else valLine = "Select to Edit";
  updateBottomMenu(name, valLine);
}

void drawTimeEdit(int h, int m, bool editingHour, String title) {
  topDisplay.clearBuffer();
  topDisplay.setFont(u8g2_font_helvB10_tr);
  topDisplay.setCursor(getCenterX(topDisplay, title), 12);
  topDisplay.print(title);
  topDisplay.setFont(u8g2_font_logisoso24_tr);
  topDisplay.setCursor(20, 50);
  if (h < 10) topDisplay.print("0");
  topDisplay.print(h);
  topDisplay.setCursor(63, 47);
  topDisplay.print(":");
  topDisplay.setCursor(75, 50);
  if (m < 10) topDisplay.print("0");
  topDisplay.print(m);
  topDisplay.setFont(u8g2_font_6x10_tf);
  if (editingHour) topDisplay.drawStr(39, 62, "^");
  else topDisplay.drawStr(94, 62, "^");
  topDisplay.sendBuffer();
  String sH = (h < 10) ? "0" + String(h) : String(h);
  String sM = (m < 10) ? "0" + String(m) : String(m);
  updateBottomMenu(title, sH + ":" + sM);
}

void drawLuxEdit(long currentLux) {
  String info = getLuxPlantType(currentLux);
  topDisplay.clearBuffer();
  topDisplay.setFont(u8g2_font_helvB10_tr);
  topDisplay.setCursor(getCenterX(topDisplay, info), 20);
  topDisplay.print(info);
  topDisplay.drawFrame(10, 30, 108, 12);
  int w = map(currentLux, 0, 120000, 0, 106);
  if (w > 106) w = 106;
  topDisplay.drawBox(11, 31, w, 10);
  topDisplay.setFont(u8g2_font_6x10_tf);
  topDisplay.setCursor(10, 55);
  topDisplay.print(currentLux);
  topDisplay.print(" lux");
  topDisplay.sendBuffer();
  updateBottomMenu("Light Threshold", String(currentLux) + " lux");
}

void drawBrightnessEdit(int level) {
  topDisplay.clearBuffer();
  topDisplay.setFont(u8g2_font_helvB10_tr);
  topDisplay.drawStr(20, 15, "BRIGHTNESS");
  topDisplay.drawFrame(10, 25, 108, 14);
  int w = map(level, 1, 10, 5, 106);
  topDisplay.drawBox(11, 26, w, 12);
  topDisplay.setFont(u8g2_font_6x10_tf);
  topDisplay.setCursor(60, 55);
  topDisplay.print(level);
  topDisplay.print("/10");
  topDisplay.sendBuffer();
  updateBottomMenu("Adjust Level", String(level));
}

void drawSensorTest() {
  topDisplay.clearBuffer();
  topDisplay.setFont(u8g2_font_helvB14_tr);
  topDisplay.drawStr(10, 30, "Sensor Test");
  topDisplay.setFont(u8g2_font_6x10_tf);
  topDisplay.drawStr(20, 50, "(Mock Mode)");
  topDisplay.sendBuffer();
  bottomDisplay.clearBuffer();
  bottomDisplay.setFont(u8g2_font_6x10_tf);
  bottomDisplay.drawStr(0, 10, "READING SENSORS...");
  bottomDisplay.drawStr(0, 20, "T: 72F  H: 45%");
  bottomDisplay.drawStr(0, 30, "L: 5400 S: 40%");
  bottomDisplay.sendBuffer();
}

void drawCircularGauge(float valLower, float valUpper, float minLimit, float maxLimit, bool editingUpper) {
  topDisplay.clearBuffer();
  int cx = 64; int cy = 32; int r_outer = 30; int r_inner = 22;
  int r_mid = (r_outer + r_inner) / 2; int r_brush = (r_outer - r_inner) / 2;
  float range = maxLimit - minLimit;
  if (range <= 0) range = 1;
  float angleStart = ((valLower - minLimit) / range) * 6.28 - 1.57;
  float angleEnd = ((valUpper - minLimit) / range) * 6.28 - 1.57;
  float a1 = min(angleStart, angleEnd); float a2 = max(angleStart, angleEnd);
  float step = 0.05;
  for (float a = a1; a <= a2; a += step) { topDisplay.drawDisc(cx + r_mid * cos(a), cy + r_mid * sin(a), r_brush); }
  topDisplay.drawDisc(cx + r_mid * cos(a2), cy + r_mid * sin(a2), r_brush);
  topDisplay.setFont(u8g2_font_4x6_tf);
  String label = editingUpper ? "UPPER" : "LOWER";
  topDisplay.setCursor(cx - (topDisplay.getStrWidth(label.c_str()) / 2), cy - 4);
  topDisplay.print(label);
  topDisplay.setFont(u8g2_font_6x10_tf);
  int val = editingUpper ? (int)valUpper : (int)valLower;
  String valStr = String(val);
  valStr += (editUnit == "F") ? "\xb0" : "%";
  topDisplay.setCursor(cx - (topDisplay.getStrWidth(valStr.c_str()) / 2), cy + 6);
  topDisplay.print(valStr);
  topDisplay.sendBuffer();
}

void updateBottomEdit(String label, float currentVal, String refLabel, float refVal) {
  bottomDisplay.clearBuffer();
  bottomDisplay.setFont(u8g2_font_6x10_tf);
  bottomDisplay.setCursor(0, 10);
  bottomDisplay.print(label);
  String refText = refLabel + String((int)refVal);
  if (editUnit == "F") refText += "\xb0"; else refText += "%";
  int wRef = bottomDisplay.getStrWidth(refText.c_str());
  bottomDisplay.setCursor(128 - wRef, 10);
  bottomDisplay.print(refText);
  bottomDisplay.setFont(u8g2_font_helvB14_tr);
  String valText = String((int)currentVal);
  if (editUnit == "F") valText += "\xb0"; else valText += "%";
  int wVal = bottomDisplay.getStrWidth(valText.c_str());
  bottomDisplay.setCursor((128 - wVal) / 2, 31);
  bottomDisplay.print(valText);
  bottomDisplay.sendBuffer();
}

/* ==========================================
 * ACTION HANDLERS
 * ========================================== */
void configModeCallback(WiFiManager* myWiFiManager) {
  topDisplay.clearBuffer();
  topDisplay.setFont(u8g2_font_6x10_tf);
  topDisplay.drawStr(10, 20, "WIFI PORTAL OPEN");
  topDisplay.drawStr(10, 35, "SSID: Plant_Setup");
  topDisplay.drawStr(10, 50, "PASS: plantadmin");
  topDisplay.sendBuffer();
  String ip = WiFi.softAPIP().toString();
  bottomDisplay.clearBuffer();
  bottomDisplay.setFont(u8g2_font_6x10_tf);
  bottomDisplay.drawStr(0, 10, "CONNECT NOW AT:");
  bottomDisplay.setFont(u8g2_font_helvB12_tr);
  bottomDisplay.drawStr(0, 30, ip.c_str());
  bottomDisplay.sendBuffer();
}

void startWiFiSetup() {
  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180);
  if (!wm.startConfigPortal("Plant_Setup", "plantadmin")) {
    updateBottomMenu("WiFi Setup", "Timed Out");
    delay(2000);
  } else {
    updateBottomMenu("Connected!", WiFi.localIP().toString());
    delay(3000);
    setupServer();
  }
  goBack();
}

void showWiFiIP() {
  String status = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "Not Connected";
  updateBottomMenu("Current IP:", status);
  delay(3000);
}

void resetWiFi() {
  WiFiManager wm;
  wm.resetSettings();
  updateBottomMenu("WiFi Reset", "Successful");
  delay(2000);
}

void startEditTemp() { uiState = STATE_EDIT_DUAL; editStep = 0; pEditVal1 = &tempLow; pEditVal2 = &tempHigh; currentMinLimit = 20.0; currentMaxLimit = 100.0; editUnit = "F"; editCurrent = *pEditVal1; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; }
void startEditHum() { uiState = STATE_EDIT_DUAL; editStep = 0; pEditVal1 = &humLow; pEditVal2 = &humHigh; currentMinLimit = 0.0; currentMaxLimit = 100.0; editUnit = "%"; editCurrent = *pEditVal1; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; }
void startEditSoil() { startEditHum(); }
void startEditSchedule() { uiState = STATE_EDIT_TIME; editStep = 0; tempOnH = timeOnHour; tempOnM = timeOnMinute; tempOffH = timeOffHour; tempOffM = timeOffMinute; editCurrent = tempOnH; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; }
void toggleTimer() { if (!timerEnabled) { timerEnabled = true; startEditSchedule(); } else { timerEnabled = false; updateBottomMenu("Timer", "Disabled"); delay(800); } }
void startSetClock() { uiState = STATE_EDIT_TIME; editStep = 10; editCurrent = currentHour; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; }
void startEditLux() { uiState = STATE_EDIT_LUX; editCurrent = luxThreshold; encoderCount = (luxThreshold / 100) * 4; lastEncoderCount = encoderCount / 4; }
void startEditBrightness() { uiState = STATE_EDIT_BRIGHTNESS; editCurrent = globalBrightness; encoderCount = globalBrightness * 4; lastEncoderCount = globalBrightness; }
void resetGlobal() { tempLow = 68.0; tempHigh = 77.0; humLow = 50.0; humHigh = 60.0; soilLow = 40; soilHigh = 60; timeOnHour = 8; timeOnMinute = 0; timeOffHour = 20; timeOffMinute = 0; luxThreshold = 50000; timerEnabled = false; globalBrightness = 10; applyBrightness(globalBrightness); updateBottomMenu("Defaults", "Restored"); delay(1500); }
void showPH() { updateBottomMenu("pH: 7.0", "Sensor OK"); delay(1000); }
void startSensorTest() { uiState = STATE_SENSOR_TEST; }

void goBack() {
  if (stackDepth == 0) { currentMenu = mainMenu; currentMenuSize = 6; selectedIndex = 0; uiState = STATE_MENU; currentHeaderName = "-- MAIN MENU --"; } 
  else {
    stackDepth--; currentMenu = menuStack[stackDepth]; currentMenuSize = menuSizeStack[stackDepth]; selectedIndex = indexStack[stackDepth]; uiState = (stackDepth == 0) ? STATE_MENU : STATE_SUBMENU;
    if (stackDepth > 0) { currentHeaderName = menuStack[stackDepth - 1][indexStack[stackDepth - 1]].name; } else { currentHeaderName = "-- MAIN MENU --"; }
  }
  menuScrollOffset = 0;
}

void enterSubMenu(MenuItem* item) {
  menuStack[stackDepth] = currentMenu; menuSizeStack[stackDepth] = currentMenuSize; indexStack[stackDepth] = selectedIndex;
  currentHeaderName = String(item->name); currentHeaderName.toUpperCase();
  stackDepth++; currentMenu = item->children; currentMenuSize = item->childCount; selectedIndex = 0; menuScrollOffset = 0; uiState = STATE_SUBMENU;
}

/* ==========================================
 * CLOUD SYNC
 * ========================================== */
void syncWithCloud() {
  if (WiFi.status() != WL_CONNECTED) return;
  updateBottomMenu("Syncing...", "Fetching Cloud");
  HTTPClient http;
  http.begin(firebaseURL);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, payload);
    if (doc.containsKey("tempLow")) tempLow = doc["tempLow"];
    if (doc.containsKey("tempHigh")) tempHigh = doc["tempHigh"];
    if (doc.containsKey("humLow")) humLow = doc["humLow"];
    if (doc.containsKey("humHigh")) humHigh = doc["humHigh"];
    if (doc.containsKey("soilLow")) soilLow = doc["soilLow"];
    if (doc.containsKey("soilHigh")) soilHigh = doc["soilHigh"];
    if (doc.containsKey("timeOnHour")) timeOnHour = doc["timeOnHour"];
    if (doc.containsKey("timeOffHour")) timeOffHour = doc["timeOffHour"];
    if (doc.containsKey("luxThreshold")) luxThreshold = doc["luxThreshold"];
    Serial.println("Cloud Sync Done!");
  }
  http.end();
}

/* ==========================================
 * WIFI SERVER HANDLERS
 * ========================================== */
void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() { addCORS(); server.send(204); }

void handleSettingsPost() {
  addCORS();
  if (server.hasArg("plain")) {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, server.arg("plain"));
    if (doc.containsKey("tempLow")) tempLow = doc["tempLow"];
    if (doc.containsKey("tempHigh")) tempHigh = doc["tempHigh"];
    if (doc.containsKey("humLow")) humLow = doc["humLow"];
    if (doc.containsKey("humHigh")) humHigh = doc["humHigh"];
    if (doc.containsKey("soilLow")) soilLow = doc["soilLow"];
    if (doc.containsKey("soilHigh")) soilHigh = doc["soilHigh"];
    if (doc.containsKey("timeOnHour")) { timeOnHour = doc["timeOnHour"]; timerEnabled = true; }
    if (doc.containsKey("timeOffHour")) { timeOffHour = doc["timeOffHour"]; timerEnabled = true; }
    if (doc.containsKey("luxThreshold")) luxThreshold = doc["luxThreshold"];
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  }
}

void setupServer() {
  server.on("/settings", HTTP_OPTIONS, handleOptions);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/", HTTP_GET, []() { addCORS(); server.send(200, "text/plain", "ESP32 is online"); });
  server.begin();
}

/* ==========================================
 * ARDUINO SETUP
 * ========================================== */
void setup() {
  Serial.begin(115200);
  pinMode(ENC_CLK, INPUT); pinMode(ENC_DT, INPUT); pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT), readEncoder, CHANGE);

  Wire.begin(I2C_SDA, I2C_SCL);
  bottomDisplay.setI2CAddress(BOTTOM_ADDR * 2); bottomDisplay.begin();
  topDisplay.setI2CAddress(TOP_ADDR * 2); topDisplay.begin();
  applyBrightness(globalBrightness);

  updateBottomMenu("Connecting WiFi...", "Please wait");
  WiFiManager wm;
  wm.setConfigPortalTimeout(15);
  if (wm.autoConnect("Plant_Setup")) {
    setupServer();
    syncWithCloud();
  } else {
    updateBottomMenu("WiFi Skipped", "Local Mode");
    delay(2000);
  }

  currentMenu = mainMenu; currentMenuSize = 6;
  lastMinuteTick = millis();
}

/* ==========================================
 * ARDUINO LOOP
 * ========================================== */
void loop() {
  updateClock();
  server.handleClient();

  // --- Encoder Logic ---
  int currentEnc = encoderCount / 4; int diff = lastEncoderCount - currentEnc;
  if (diff != 0) {
    lastEncoderCount = currentEnc;
    if (uiState == STATE_MENU || uiState == STATE_SUBMENU) {
      selectedIndex += diff;
      if (selectedIndex < 0) selectedIndex = currentMenuSize - 1;
      if (selectedIndex >= currentMenuSize) selectedIndex = 0;
      if (selectedIndex < menuScrollOffset) menuScrollOffset = selectedIndex;
      if (selectedIndex >= menuScrollOffset + VISIBLE_ITEMS) menuScrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
    } 
    else if (uiState == STATE_EDIT_DUAL) {
      editCurrent += diff;
      if (editStep == 0) { 
        if (editCurrent < currentMinLimit) editCurrent = currentMinLimit; 
        if (editCurrent > currentMaxLimit) editCurrent = currentMaxLimit;
        if (editCurrent > *pEditVal2) *pEditVal2 = editCurrent;
      } else { 
        if (editCurrent < *pEditVal1) editCurrent = *pEditVal1; 
        if (editCurrent > currentMaxLimit) editCurrent = currentMaxLimit; 
      }
    }
    else if (uiState == STATE_EDIT_TIME) {
      editCurrent += diff;
      int limit = (editStep == 0 || editStep == 2 || editStep == 10) ? 23 : 59;
      if (editCurrent < 0) editCurrent = limit;
      if (editCurrent > limit) editCurrent = 0;
    }
    else if (uiState == STATE_EDIT_LUX) {
      long step = (abs(diff) > 1) ? 500 : 100; editCurrent += (diff * step);
      if (editCurrent < 0) editCurrent = 0;
      if (editCurrent > 120000) editCurrent = 120000;
    }
    else if (uiState == STATE_EDIT_BRIGHTNESS) {
      editCurrent += diff;
      if (editCurrent < 1) editCurrent = 1; if (editCurrent > 10) editCurrent = 10;
      globalBrightness = (int)editCurrent; applyBrightness(globalBrightness);
    }
  }

  // --- Button Logic ---
  static unsigned long lastBtnTime = 0; bool clicked = false;
  if (digitalRead(ENC_SW) == LOW) { if (millis() - lastBtnTime > 250) { clicked = true; lastBtnTime = millis(); } }

  // --- UI Rendering ---
  if (uiState == STATE_MENU || uiState == STATE_SUBMENU) {
    topDisplay.clearBuffer();
    topDisplay.setFont(u8g2_font_6x10_tf);
    int headerX = getCenterX(topDisplay, currentHeaderName);
    topDisplay.drawStr(headerX, 10, currentHeaderName.c_str());
    topDisplay.drawHLine(0, 12, 128);
    for (int i = 0; i < currentMenuSize; i++) {
      if (i >= menuScrollOffset && i < menuScrollOffset + VISIBLE_ITEMS) {
        int y = 28 + ((i - menuScrollOffset) * 12);
        if (i == selectedIndex) {
          topDisplay.setFont(u8g2_font_7x14B_tf);
          String label = currentMenu[i].name;
          if (label.startsWith("Timer")) label = timerEnabled ? "Timer: ON" : "Timer: OFF";
          topDisplay.drawStr(0, y, ">"); topDisplay.drawStr(10, y, label.c_str());
        } else {
          topDisplay.setFont(u8g2_font_6x10_tf);
          String label = currentMenu[i].name;
          if (label.startsWith("Timer")) label = timerEnabled ? "Timer: ON" : "Timer: OFF";
          topDisplay.drawStr(10, y, label.c_str());
        }
      }
    }
    topDisplay.sendBuffer();

    static int lastIdx = -1;
    if (selectedIndex != lastIdx) { showHoverContext(currentMenu[selectedIndex].name); lastIdx = selectedIndex; }

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
    drawCircularGauge(valL, valH, currentMinLimit, currentMaxLimit, (editStep == 1));
    static float lastValDisp = -999; static int lastStepDisp = -1;
    if (lastValDisp != editCurrent || lastStepDisp != editStep || (editStep == 0 && *pEditVal2 == editCurrent)) {
      if (editStep == 0) updateBottomEdit("SET LOWER", editCurrent, "High: ", *pEditVal2);
      else updateBottomEdit("SET UPPER", editCurrent, "Low: ", *pEditVal1);
      lastValDisp = editCurrent; lastStepDisp = editStep;
    }
    if (clicked) {
      if (editStep == 0) { *pEditVal1 = editCurrent; editStep = 1; editCurrent = *pEditVal2; encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; } 
      else { *pEditVal2 = editCurrent; updateBottomMenu("RANGE", "SAVED"); delay(800); goBack(); }
    }
  }
  else if (uiState == STATE_EDIT_TIME) {
    if (editStep < 10) {
      String title = (editStep < 2) ? "TURN ON TIME" : "TURN OFF TIME";
      bool isHour = (editStep == 0 || editStep == 2);
      int showH = (editStep == 0) ? (int)editCurrent : (editStep == 1) ? tempOnH : (editStep == 2) ? (int)editCurrent : tempOffH;
      int showM = (editStep == 0) ? tempOnM : (editStep == 1) ? (int)editCurrent : (editStep == 2) ? tempOffM : (int)editCurrent;
      drawTimeEdit(showH, showM, isHour, title);
      if (clicked) {
        if (editStep == 0) { tempOnH = (int)editCurrent; editStep = 1; editCurrent = tempOnM; } 
        else if (editStep == 1) { tempOnM = (int)editCurrent; editStep = 2; editCurrent = tempOffH; } 
        else if (editStep == 2) { tempOffH = (int)editCurrent; editStep = 3; editCurrent = tempOffM; } 
        else { tempOffM = (int)editCurrent; timeOnHour = tempOnH; timeOnMinute = tempOnM; timeOffHour = tempOffH; timeOffMinute = tempOffM; updateBottomMenu("SCHEDULE", "SAVED"); delay(800); goBack(); }
        encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; delay(200);
      }
    } else {
      String title = "SET CLOCK"; bool isHour = (editStep == 10);
      int showH = (editStep == 10) ? (int)editCurrent : currentHour;
      int showM = (editStep == 11) ? (int)editCurrent : currentMinute;
      drawTimeEdit(showH, showM, isHour, title);
      if (clicked) {
        if (editStep == 10) { currentHour = (int)editCurrent; editStep = 11; editCurrent = currentMinute; } 
        else { currentMinute = (int)editCurrent; updateBottomMenu("CLOCK", "UPDATED"); delay(800); goBack(); }
        encoderCount = (int)editCurrent * 4; lastEncoderCount = (int)editCurrent; delay(200);
      }
    }
  }
  else if (uiState == STATE_EDIT_LUX) {
    drawLuxEdit((long)editCurrent);
    if (clicked) { luxThreshold = (long)editCurrent; updateBottomMenu("LUX LIMIT", "SAVED"); delay(800); goBack(); }
  } 
  else if (uiState == STATE_EDIT_BRIGHTNESS) {
    drawBrightnessEdit((int)editCurrent);
    if (clicked) { updateBottomMenu("BRIGHTNESS", "SAVED"); delay(800); goBack(); }
  } 
  else if (uiState == STATE_SENSOR_TEST) {
    drawSensorTest();
    if (clicked) goBack();
  }
}