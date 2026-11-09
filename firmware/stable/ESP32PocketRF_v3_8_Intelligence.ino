#include <Arduino.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#define TOUCH_MODULES_CST_SELF
#include <TouchLib.h>
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <Preferences.h>

// ==================== HARDWARE ====================
#define LED_1 11
#define LED_2 12
#define LED_3 13
#define LED_STATUS 43
#define BUTTON_1 0
#define BUTTON_2 14
#define TFT_POWER 15
#define BATTERY_PIN 4
#define TOUCH_SDA 18
#define TOUCH_SCL 17
#define TOUCH_RST 21
#define TOUCH_INT 16

// ==================== CONFIG ====================
#define MAX_DEVICES 30
#define VISIBLE_ROWS 4
#define INITIAL_SCAN_SECONDS 6
#define TRACK_SCAN_SECONDS 1
#define BATTERY_UPDATE_MS 10000
#define DISPLAY_UPDATE_MS 250
#define POWER_OFF_HOLD_MS 1500
#define TOUCH_COOLDOWN_MS 140
#define AUTO_RESCAN_MS 20000
#define SCREEN_OFF_MS 90000
#define RSSI_HISTORY_SIZE 30
#define HEADER_HEIGHT 28
#define LIST_TOP 29
#define ROW_HEIGHT 24
#define ROW_SPACING 1
#define LIST_WIDTH 270
#define ARROW_X 273
#define ARROW_WIDTH 45
#define UP_ARROW_Y 29
#define UP_ARROW_HEIGHT 49
#define DOWN_ARROW_Y 80
#define DOWN_ARROW_HEIGHT 49
#define BUTTON_TOP 130
#define BUTTON_HEIGHT 40
#define ACTION_WIDTH 88
#define RAW_X_MIN 0
#define RAW_X_MAX 180
#define RAW_Y_MIN 0
#define RAW_Y_MAX 330

// ==================== OBJECTS ====================
TFT_eSPI tft = TFT_eSPI();
TouchLib touch(Wire, TOUCH_SDA, TOUCH_SCL, CTS820_SLAVE_ADDRESS, TOUCH_RST);
BLEScan *bleScanner;
Preferences preferences;

// ==================== COLORS ====================
#define COLOR_HEADER 0x0410
#define COLOR_SELECTED 0x29A5
#define COLOR_BUTTON 0x18E3
#define COLOR_BUTTON_ACTIVE 0x04D3
#define COLOR_MUTED 0x8410
#define COLOR_KNOWN 0x07E0
#define COLOR_UNKNOWN 0xFD20
#define COLOR_ARROW 0x2124
#define COLOR_ARROW_DISABLED 0x4208

// ==================== STATE ====================
enum ScreenMode { SCREEN_DEVICE_LIST, SCREEN_TRACKING, SCREEN_DEVICE_DETAILS };
ScreenMode currentScreen = SCREEN_DEVICE_LIST;

enum StatusLEDMode {
  STATUS_BOOTING, STATUS_READY, STATUS_SCANNING,
  STATUS_TRACKING, STATUS_TARGET_LOST, STATUS_SLEEPING
};
volatile StatusLEDMode statusLEDMode = STATUS_BOOTING;
TaskHandle_t statusLEDTaskHandle = NULL;

struct DeviceRecord {
  String name;
  String address;
  String manufacturer;
  uint16_t manufacturerID;
  int rssi;
  int strongestRSSI;
  int weakestRSSI;
  unsigned long firstSeen;
  unsigned long lastSeen;
  unsigned long observationCount;
  bool known;
  bool hasAdvertisedName;
  String deviceType;
};

DeviceRecord devices[MAX_DEVICES];
int deviceCount = 0;
int selectedDevice = 0;
int firstVisibleDevice = 0;
String knownAddresses = "";

volatile bool tracking = false;
volatile bool trackingTaskBusy = false;
volatile bool newTrackingResult = false;
volatile bool latestTargetFound = false;
volatile int latestRawRSSI = -100;
portMUX_TYPE trackingMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t trackingTaskHandle = NULL;
String targetAddress = "";
String targetName = "";
String targetManufacturer = "";
float smoothedRSSI = -100.0;
int targetPeakRSSI = -100;
unsigned long targetObservations = 0;
unsigned long lastDisplayUpdate = 0;

uint32_t batteryMillivolts = 0;
int batteryPercent = 0;
bool usbPowerDetected = false;
unsigned long lastBatteryUpdate = 0;

bool touchActive = false;
int touchStartX = 0, touchStartY = 0;
int touchCurrentX = 0, touchCurrentY = 0;
unsigned long lastTouchActionAt = 0;
bool powerCombinationActive = false;
unsigned long powerCombinationStarted = 0;
unsigned long lastAutoRescan = 0;
unsigned long lastUserActivity = 0;
bool screenIsOff = false;
int rssiHistory[RSSI_HISTORY_SIZE];
int rssiHistoryCount = 0;
int rssiHistoryWrite = 0;

void displayDeviceList();
void displayTrackingScreen(int rssi, bool found);
void displayDeviceDetails();

// ==================== STATUS LED ====================
void statusLEDTask(void *parameter) {
  bool state = false;
  while (true) {
    switch (statusLEDMode) {
      case STATUS_BOOTING:
        state = !state; digitalWrite(LED_STATUS, state); vTaskDelay(pdMS_TO_TICKS(500)); break;
      case STATUS_SCANNING:
        state = !state; digitalWrite(LED_STATUS, state); vTaskDelay(pdMS_TO_TICKS(250)); break;
      case STATUS_TRACKING:
        state = true; digitalWrite(LED_STATUS, HIGH); vTaskDelay(pdMS_TO_TICKS(100)); break;
      case STATUS_TARGET_LOST:
        state = !state; digitalWrite(LED_STATUS, state); vTaskDelay(pdMS_TO_TICKS(100)); break;
      default:
        state = false; digitalWrite(LED_STATUS, LOW); vTaskDelay(pdMS_TO_TICKS(100)); break;
    }
  }
}

// ==================== NAME PARSING ====================
String cleanBLEName(String value) {
  value.trim();
  String cleaned = "";
  for (unsigned int i = 0; i < value.length(); i++) {
    uint8_t c = (uint8_t)value[i];
    if (c >= 32 && c <= 126) cleaned += (char)c;
  }
  cleaned.trim();
  return cleaned;
}

String extractLocalNameFromPayload(BLEAdvertisedDevice &device) {
  uint8_t *payload = device.getPayload();
  size_t payloadLength = device.getPayloadLength();
  if (payload == nullptr || payloadLength < 2) return "";

  String shortName = "";
  size_t index = 0;
  while (index < payloadLength) {
    uint8_t fieldLength = payload[index];
    if (fieldLength == 0) break;
    size_t next = index + fieldLength + 1;
    if (next > payloadLength || fieldLength < 1) break;

    uint8_t type = payload[index + 1];
    size_t dataLength = fieldLength - 1;
    if ((type == 0x08 || type == 0x09) && dataLength > 0) {
      String parsed = "";
      for (size_t i = 0; i < dataLength; i++) {
        uint8_t c = payload[index + 2 + i];
        if (c == 0) break;
        if (c >= 32 && c <= 126) parsed += (char)c;
      }
      parsed = cleanBLEName(parsed);
      if (parsed.length() > 0) {
        if (type == 0x09) return parsed; // Complete Local Name wins.
        shortName = parsed;             // Save Shortened Local Name.
      }
    }
    index = next;
  }
  return shortName;
}

String bestAdvertisedName(BLEAdvertisedDevice &device) {
  if (device.haveName()) {
    String direct = cleanBLEName(String(device.getName().c_str()));
    if (direct.length() > 0) return direct;
  }
  return extractLocalNameFromPayload(device);
}

String shortBLEAddress(String address) {
  return address.length() >= 8 ? address.substring(address.length() - 8) : address;
}

String makeFallbackDeviceName(String manufacturer, String address) {
  String suffix = shortBLEAddress(address);
  if (manufacturer.length() && manufacturer != "Not advertised") return manufacturer + " " + suffix;
  return "BLE " + suffix;
}


String classifyDevice(BLEAdvertisedDevice &device, String name, String manufacturer) {
  String n = name; n.toLowerCase();
  if (n.indexOf("tv") >= 0 || n.indexOf("display") >= 0 || n.indexOf("roku") >= 0 || n.indexOf("chromecast") >= 0)
    return "Display / TV";
  if (n.indexOf("airpod") >= 0 || n.indexOf("buds") >= 0 || n.indexOf("headphone") >= 0 ||
      n.indexOf("speaker") >= 0 || n.indexOf("jbl") >= 0 || n.indexOf("sound") >= 0)
    return "Audio";
  if (n.indexOf("tag") >= 0 || n.indexOf("tile") >= 0 || n.indexOf("beacon") >= 0)
    return "Beacon / Tag";
  if (n.indexOf("keyboard") >= 0 || n.indexOf("mouse") >= 0 || n.indexOf("remote") >= 0 || n.indexOf("controller") >= 0)
    return "Input Device";
  if (n.indexOf("watch") >= 0 || n.indexOf("fitbit") >= 0 || n.indexOf("band") >= 0)
    return "Wearable";
  if (n.indexOf("sensor") >= 0 || n.indexOf("thermo") >= 0 || n.indexOf("meter") >= 0)
    return "Sensor";
  if (n.indexOf("iphone") >= 0 || n.indexOf("phone") >= 0 || n.indexOf("galaxy") >= 0)
    return "Phone";
  if (n.indexOf("macbook") >= 0 || n.indexOf("laptop") >= 0 || n.indexOf("computer") >= 0 || n.indexOf("pc") >= 0)
    return "Computer";
  if (manufacturer == "Apple") return "Apple BLE Device";
  if (manufacturer == "Samsung") return "Samsung BLE Device";
  if (manufacturer == "Microsoft") return "Microsoft BLE Device";
  return "BLE Device";
}

void addRSSIHistory(int rssi) {
  rssiHistory[rssiHistoryWrite] = rssi;
  rssiHistoryWrite = (rssiHistoryWrite + 1) % RSSI_HISTORY_SIZE;
  if (rssiHistoryCount < RSSI_HISTORY_SIZE) rssiHistoryCount++;
}

String rssiTrend() {
  if (rssiHistoryCount < 6) return "COLLECTING";
  int recent = 0, older = 0;
  for (int i = 0; i < 3; i++) {
    int ri = (rssiHistoryWrite - 1 - i + RSSI_HISTORY_SIZE) % RSSI_HISTORY_SIZE;
    int oi = (rssiHistoryWrite - 4 - i + RSSI_HISTORY_SIZE) % RSSI_HISTORY_SIZE;
    recent += rssiHistory[ri]; older += rssiHistory[oi];
  }
  int delta = recent / 3 - older / 3;
  if (delta >= 3) return "GETTING CLOSER";
  if (delta <= -3) return "GETTING WEAKER";
  return "STEADY";
}

void drawRSSIHistory() {
  const int gx = 104, gy = 98, gw = 204, gh = 18;
  tft.fillRect(gx, gy, gw, gh, TFT_BLACK);
  tft.drawRect(gx, gy, gw, gh, TFT_DARKGREY);
  if (rssiHistoryCount < 2) return;
  int first = (rssiHistoryWrite - rssiHistoryCount + RSSI_HISTORY_SIZE) % RSSI_HISTORY_SIZE;
  int previousX = gx + 2;
  int previousRssi = constrain(rssiHistory[first], -100, -35);
  int previousY = gy + gh - 2 - map(previousRssi, -100, -35, 0, gh - 4);
  for (int i = 1; i < rssiHistoryCount; i++) {
    int idx = (first + i) % RSSI_HISTORY_SIZE;
    int x = gx + 2 + map(i, 0, RSSI_HISTORY_SIZE - 1, 0, gw - 4);
    int value = constrain(rssiHistory[idx], -100, -35);
    int y = gy + gh - 2 - map(value, -100, -35, 0, gh - 4);
    tft.drawLine(previousX, previousY, x, y, TFT_CYAN);
    previousX = x; previousY = y;
  }
}

void wakeDisplay() {
  if (!screenIsOff) return;
  digitalWrite(TFT_POWER, HIGH);
  delay(20);
  tft.writecommand(0x11);
  delay(120);
  tft.writecommand(0x29);
  screenIsOff = false;
  lastUserActivity = millis();
  if (currentScreen == SCREEN_DEVICE_LIST) displayDeviceList();
  else if (currentScreen == SCREEN_DEVICE_DETAILS) displayDeviceDetails();
  else displayTrackingScreen((int)round(smoothedRSSI), latestTargetFound);
}

void noteUserActivity() {
  if (screenIsOff) wakeDisplay();
  lastUserActivity = millis();
}

void manageScreenPower() {
  if (!screenIsOff && millis() - lastUserActivity >= SCREEN_OFF_MS) {
    tft.writecommand(0x28);
    screenIsOff = true;
  }
}

// ==================== KNOWN DEVICES ====================
void loadKnownDevices() {
  preferences.begin("pocketrf", false);
  knownAddresses = preferences.getString("known", "");
}
void saveKnownDevices() { preferences.putString("known", knownAddresses); }
bool isAddressKnown(String address) { return knownAddresses.indexOf("|" + address + "|") >= 0; }
void toggleKnownDevice(int index) {
  if (index < 0 || index >= deviceCount) return;
  String token = "|" + devices[index].address + "|";
  if (knownAddresses.indexOf(token) >= 0) {
    knownAddresses.replace(token, ""); devices[index].known = false;
  } else {
    knownAddresses += token; devices[index].known = true;
  }
  saveKnownDevices();
}

// ==================== LEDS ====================
void ledsOff() { digitalWrite(LED_1, LOW); digitalWrite(LED_2, LOW); digitalWrite(LED_3, LOW); }
void updateProximityLEDs(int rssi, bool found) {
  ledsOff();
  if (!found) return;
  if (rssi >= -85) digitalWrite(LED_1, HIGH);
  if (rssi >= -70) digitalWrite(LED_2, HIGH);
  if (rssi >= -55) digitalWrite(LED_3, HIGH);
}

// ==================== BATTERY ====================
int calculateBatteryPercent(uint32_t mv) {
  if (mv >= 4200) return 100;
  if (mv >= 4100) return map(mv, 4100, 4200, 90, 100);
  if (mv >= 4000) return map(mv, 4000, 4100, 75, 90);
  if (mv >= 3900) return map(mv, 3900, 4000, 55, 75);
  if (mv >= 3800) return map(mv, 3800, 3900, 35, 55);
  if (mv >= 3700) return map(mv, 3700, 3800, 15, 35);
  if (mv >= 3500) return map(mv, 3500, 3700, 3, 15);
  return 0;
}
void readBatteryLevel() {
  uint32_t total = 0;
  for (int i = 0; i < 12; i++) { total += analogReadMilliVolts(BATTERY_PIN); delay(2); }
  batteryMillivolts = (total / 12) * 2;
  usbPowerDetected = batteryMillivolts > 4350;
  if (!usbPowerDetected) batteryPercent = constrain(calculateBatteryPercent(batteryMillivolts), 0, 100);
  lastBatteryUpdate = millis();
}
uint16_t batteryColor() {
  if (usbPowerDetected) return TFT_CYAN;
  if (batteryPercent > 50) return TFT_GREEN;
  if (batteryPercent > 20) return TFT_YELLOW;
  return TFT_RED;
}
void drawBatteryIndicator() {
  int x = 253, y = 7;
  tft.fillRect(244, 2, 76, 24, COLOR_HEADER);
  tft.drawRect(x, y, 24, 12, TFT_WHITE);
  tft.fillRect(x + 24, y + 3, 3, 6, TFT_WHITE);
  int fillWidth = usbPowerDetected ? 20 : map(batteryPercent, 0, 100, 0, 20);
  tft.fillRect(x + 2, y + 2, constrain(fillWidth, 0, 20), 8, batteryColor());
  tft.setTextColor(TFT_WHITE, COLOR_HEADER); tft.setTextSize(1); tft.setCursor(283, 9);
  if (usbPowerDetected) tft.print("USB"); else tft.printf("%d%%", batteryPercent);
}

// ==================== DISPLAY HELPERS ====================
String shortenedText(String text, int maxLength) {
  if (!text.length()) return "BLE device";
  return text.length() > maxLength ? text.substring(0, maxLength - 1) + "." : text;
}
String proximityDescription(int rssi) {
  if (rssi >= -45) return "EXTREMELY CLOSE";
  if (rssi >= -55) return "VERY CLOSE";
  if (rssi >= -65) return "CLOSE";
  if (rssi >= -75) return "NEARBY";
  if (rssi >= -85) return "WEAK";
  return "VERY WEAK";
}
uint16_t proximityColor(int rssi) {
  if (rssi >= -55) return TFT_GREEN;
  if (rssi >= -70) return TFT_YELLOW;
  if (rssi >= -85) return TFT_ORANGE;
  return TFT_RED;
}
void drawHeader(String title) {
  tft.fillScreen(TFT_BLACK); tft.fillRect(0, 0, 320, HEADER_HEIGHT, COLOR_HEADER);
  tft.setTextColor(TFT_WHITE, COLOR_HEADER); tft.setTextSize(2); tft.setCursor(7, 6); tft.print(title);
  drawBatteryIndicator();
}
void drawTouchButton(int x, int y, int w, int h, String label, uint16_t color, int textSize) {
  tft.fillRoundRect(x, y, w, h, 6, color); tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, color); tft.setTextSize(textSize);
  tft.drawString(label, x + w / 2, y + h / 2); tft.setTextDatum(TL_DATUM);
}
bool pointInside(int px, int py, int x, int y, int w, int h) {
  return px >= x && px < x + w && py >= y && py < y + h;
}
void showStartupScreen() {
  tft.fillScreen(TFT_BLACK); tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.setTextSize(3); tft.drawString("Matt's C3", 160, 45);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(2); tft.drawString("BLE Prox Tracker", 160, 84);
  tft.setTextColor(COLOR_MUTED, TFT_BLACK); tft.setTextSize(1); tft.drawString("Intelligence v3.8", 160, 119);
  tft.setTextDatum(TL_DATUM); delay(1800);
}

// ==================== MANUFACTURER ====================
String manufacturerName(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple"; case 0x0006: return "Microsoft"; case 0x00E0: return "Google";
    case 0x0075: return "Samsung"; case 0x0059: return "Nordic"; case 0x02E5: return "Espressif";
    case 0x000F: return "Broadcom"; case 0x0131: return "Cypress"; case 0x015D: return "Amazon";
    case 0x038F: return "Xiaomi";
  }
  if (id == 0xFFFF) return "Not advertised";
  char out[24]; snprintf(out, sizeof(out), "Company 0x%04X", id); return String(out);
}
uint16_t getManufacturerID(BLEAdvertisedDevice &device) {
  if (!device.haveManufacturerData()) return 0xFFFF;
  String data = device.getManufacturerData();
  if (data.length() < 2) return 0xFFFF;
  return (uint8_t)data[0] | ((uint8_t)data[1] << 8);
}

// ==================== DEVICE DATABASE ====================
int findDeviceByAddress(String address) {
  for (int i = 0; i < deviceCount; i++) if (devices[i].address.equalsIgnoreCase(address)) return i;
  return -1;
}

void updateDeviceRecord(BLEAdvertisedDevice &device) {
  String address = String(device.getAddress().toString().c_str());
  int index = findDeviceByAddress(address);
  int currentRSSI = device.getRSSI();

  if (index < 0) {
    if (deviceCount >= MAX_DEVICES) return;
    index = deviceCount++;
    devices[index].address = address;
    devices[index].manufacturer = "Not advertised";
    devices[index].manufacturerID = 0xFFFF;
    devices[index].name = "BLE " + shortBLEAddress(address);
    devices[index].hasAdvertisedName = false;
    devices[index].deviceType = "BLE Device";
    devices[index].rssi = currentRSSI;
    devices[index].strongestRSSI = currentRSSI;
    devices[index].weakestRSSI = currentRSSI;
    devices[index].firstSeen = millis();
    devices[index].lastSeen = millis();
    devices[index].observationCount = 0;
    devices[index].known = isAddressKnown(address);
  }

  uint16_t companyID = getManufacturerID(device);
  if (companyID != 0xFFFF) {
    devices[index].manufacturerID = companyID;
    devices[index].manufacturer = manufacturerName(companyID);
  }

  String discoveredName = bestAdvertisedName(device);
  if (discoveredName.length() > 0) {
    devices[index].name = discoveredName;
    devices[index].hasAdvertisedName = true;
  } else if (!devices[index].hasAdvertisedName) {
    devices[index].name = makeFallbackDeviceName(devices[index].manufacturer, address);
  }

  devices[index].deviceType = classifyDevice(
    device,
    devices[index].name,
    devices[index].manufacturer
  );

  devices[index].rssi = currentRSSI;
  devices[index].lastSeen = millis();
  devices[index].observationCount++;
  devices[index].known = isAddressKnown(address);
  if (currentRSSI > devices[index].strongestRSSI) devices[index].strongestRSSI = currentRSSI;
  if (currentRSSI < devices[index].weakestRSSI) devices[index].weakestRSSI = currentRSSI;
}

void sortDevicesByRSSI() {
  for (int i = 0; i < deviceCount - 1; i++) {
    for (int j = i + 1; j < deviceCount; j++) {
      if (devices[j].rssi > devices[i].rssi) {
        DeviceRecord tmp = devices[i]; devices[i] = devices[j]; devices[j] = tmp;
      }
    }
  }
}

// Callback captures repeated advertisements and scan responses during active scanning.
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (!tracking) updateDeviceRecord(advertisedDevice);
  }
};
ScanCallbacks scanCallbacks;

// ==================== LIST NAVIGATION ====================
void clampListPosition() {
  firstVisibleDevice = constrain(firstVisibleDevice, 0, max(0, deviceCount - VISIBLE_ROWS));
}
void moveListUp() {
  if (firstVisibleDevice <= 0) return;
  firstVisibleDevice--;
  int lastVisible = firstVisibleDevice + VISIBLE_ROWS - 1;
  if (selectedDevice > lastVisible) selectedDevice = lastVisible;
  displayDeviceList();
}
void moveListDown() {
  int maxFirst = max(0, deviceCount - VISIBLE_ROWS);
  if (firstVisibleDevice >= maxFirst) return;
  firstVisibleDevice++;
  if (selectedDevice < firstVisibleDevice) selectedDevice = firstVisibleDevice;
  displayDeviceList();
}

// ==================== SCANNING ====================
void scanForDevices(bool clearHistory) {
  String preserveAddress = "";
  if (!clearHistory && selectedDevice >= 0 && selectedDevice < deviceCount)
    preserveAddress = devices[selectedDevice].address;

  tracking = false; ledsOff(); statusLEDMode = STATUS_SCANNING;
  if (!screenIsOff) {
    drawHeader("SCANNING");
    tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.setTextSize(2);
    tft.drawString("Scanning BLE devices", 160, 70);
    tft.setTextColor(COLOR_MUTED, TFT_BLACK); tft.setTextSize(1);
    tft.drawString("Merging names + responses", 160, 100); tft.setTextDatum(TL_DATUM);
  }

  if (clearHistory) deviceCount = 0;
  bleScanner->clearResults();
  bleScanner->start(INITIAL_SCAN_SECONDS, false);
  bleScanner->clearResults();
  sortDevicesByRSSI();

  selectedDevice = 0;
  if (preserveAddress.length()) {
    int preserved = findDeviceByAddress(preserveAddress);
    if (preserved >= 0) selectedDevice = preserved;
  }
  firstVisibleDevice = max(0, selectedDevice - VISIBLE_ROWS + 1);
  clampListPosition();
  statusLEDMode = STATUS_READY;
  lastAutoRescan = millis();
}

// ==================== DEVICE LIST ====================
void displayDeviceList() {
  currentScreen = SCREEN_DEVICE_LIST; statusLEDMode = STATUS_READY; clampListPosition(); drawHeader("BLE DEVICES");
  int startNum = deviceCount ? firstVisibleDevice + 1 : 0;
  int endNum = deviceCount ? min(firstVisibleDevice + VISIBLE_ROWS, deviceCount) : 0;
  tft.fillRect(164, 4, 77, 20, COLOR_HEADER); tft.setTextColor(TFT_LIGHTGREY, COLOR_HEADER);
  tft.setTextSize(1); tft.setCursor(169, 10); tft.printf("%d-%d/%d", startNum, endNum, deviceCount);

  for (int row = 0; row < VISIBLE_ROWS; row++) {
    int index = firstVisibleDevice + row; if (index >= deviceCount) break;
    int y = LIST_TOP + row * (ROW_HEIGHT + ROW_SPACING);
    bool selected = index == selectedDevice;
    uint16_t bg = selected ? COLOR_SELECTED : TFT_BLACK;
    tft.fillRoundRect(1, y, LIST_WIDTH, ROW_HEIGHT, 3, bg);
    DeviceRecord &d = devices[index];
    tft.fillCircle(9, y + 8, 4, d.known ? COLOR_KNOWN : COLOR_UNKNOWN);
    tft.setTextColor(TFT_WHITE, bg); tft.setTextSize(1); tft.setCursor(18, y + 3);
    tft.print(shortenedText(d.name, 18));
    tft.setTextColor(proximityColor(d.rssi), bg); tft.setCursor(226, y + 3); tft.printf("%d", d.rssi);
    tft.setTextColor(COLOR_MUTED, bg); tft.setCursor(18, y + 14);
    tft.print(shortenedText(d.manufacturer, 18));
    tft.setCursor(220, y + 14); tft.print(d.known ? "KNOWN" : "NEW");
  }

  bool up = firstVisibleDevice > 0;
  bool down = firstVisibleDevice < max(0, deviceCount - VISIBLE_ROWS);
  drawTouchButton(ARROW_X, UP_ARROW_Y, ARROW_WIDTH, UP_ARROW_HEIGHT, "^", up ? COLOR_ARROW : COLOR_ARROW_DISABLED, 3);
  drawTouchButton(ARROW_X, DOWN_ARROW_Y, ARROW_WIDTH, DOWN_ARROW_HEIGHT, "v", down ? COLOR_ARROW : COLOR_ARROW_DISABLED, 3);
  drawTouchButton(1, BUTTON_TOP, ACTION_WIDTH, BUTTON_HEIGHT, "SCAN", COLOR_BUTTON, 1);
  drawTouchButton(91, BUTTON_TOP, ACTION_WIDTH, BUTTON_HEIGHT, "TAG", COLOR_BUTTON, 1);
  drawTouchButton(181, BUTTON_TOP, ACTION_WIDTH, BUTTON_HEIGHT, "TRACK", COLOR_BUTTON_ACTIVE, 1);
}

void displayDeviceDetails() {
  if (selectedDevice < 0 || selectedDevice >= deviceCount) return;
  currentScreen = SCREEN_DEVICE_DETAILS;
  DeviceRecord &d = devices[selectedDevice];
  drawHeader("DEVICE DETAILS");
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.setCursor(8, 34);
  tft.print(shortenedText(d.name, 38));
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 50); tft.print("Type: "); tft.print(d.deviceType);
  tft.setCursor(8, 62); tft.print("Maker: "); tft.print(shortenedText(d.manufacturer, 31));
  tft.setCursor(8, 74); tft.print("MAC: "); tft.print(d.address);
  tft.setCursor(8, 86); tft.printf("RSSI: %d  Peak: %d  Low: %d", d.rssi, d.strongestRSSI, d.weakestRSSI);
  tft.setCursor(8, 98); tft.printf("Seen: %lu   %s", d.observationCount, d.known ? "KNOWN" : "NEW");
  tft.setCursor(8, 110); tft.print(d.hasAdvertisedName ? "Name source: BLE broadcast" : "Name source: fallback");
  drawTouchButton(2, BUTTON_TOP, 316, BUTTON_HEIGHT, "< BACK", COLOR_BUTTON_ACTIVE, 2);
}

// ==================== TRACKING UI ====================
void displayTrackingScreen(int rssi, bool found) {
  currentScreen = SCREEN_TRACKING;
  statusLEDMode = found ? STATUS_TRACKING : STATUS_TARGET_LOST;
  drawHeader("PROXIMITY");
  tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.setTextSize(2); tft.setCursor(8, 34);
  tft.print(shortenedText(targetName, 22));
  tft.setTextColor(COLOR_MUTED, TFT_BLACK); tft.setTextSize(1); tft.setCursor(8, 55);
  tft.print(shortenedText(targetManufacturer, 27));

  if (!found) {
    tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_RED, TFT_BLACK); tft.setTextSize(2);
    tft.drawString("TARGET NOT SEEN", 160, 82);
    tft.setTextColor(COLOR_MUTED, TFT_BLACK); tft.setTextSize(1);
    tft.drawString(trackingTaskBusy ? "Scanning in background" : "Waiting for next scan", 160, 108);
    tft.setTextDatum(TL_DATUM); updateProximityLEDs(-100, false);
  } else {
    uint16_t color = proximityColor(rssi);
    tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(3); tft.setCursor(8, 68); tft.printf("%d", rssi);
    tft.setTextSize(1); tft.setCursor(81, 81); tft.print("dBm");
    tft.setTextColor(color, TFT_BLACK); tft.setTextSize(2); tft.setCursor(121, 74);
    tft.print(proximityDescription(rssi));
    int bar = map(constrain(rssi, -100, -35), -100, -35, 4, 296);
    tft.drawRect(8, 101, 90, 13, TFT_WHITE); tft.fillRect(10, 103, map(constrain(rssi, -100, -35), -100, -35, 2, 86), 9, color);
    drawRSSIHistory();
    tft.setTextColor(color, TFT_BLACK); tft.setTextSize(1); tft.setCursor(121, 88); tft.print(rssiTrend());
    tft.setTextColor(COLOR_MUTED, TFT_BLACK); tft.setTextSize(1); tft.setCursor(8, 119);
    tft.printf("Peak %d dBm  Seen %lu", targetPeakRSSI, targetObservations);
    updateProximityLEDs(rssi, true);
  }
  drawTouchButton(2, BUTTON_TOP, 156, BUTTON_HEIGHT, "< BACK", COLOR_BUTTON_ACTIVE, 2);
  drawTouchButton(162, BUTTON_TOP, 156, BUTTON_HEIGHT, "REFRESH", COLOR_BUTTON, 2);
}

void startTrackingSelectedDevice() {
  if (deviceCount <= 0 || selectedDevice < 0 || selectedDevice >= deviceCount) return;
  DeviceRecord &d = devices[selectedDevice];
  targetAddress = d.address; targetName = d.name; targetManufacturer = d.manufacturer;
  smoothedRSSI = d.rssi; targetPeakRSSI = d.strongestRSSI; targetObservations = d.observationCount;
  portENTER_CRITICAL(&trackingMux);
  newTrackingResult = false; latestTargetFound = false; latestRawRSSI = -100;
  portEXIT_CRITICAL(&trackingMux);
  rssiHistoryCount = 0; rssiHistoryWrite = 0;
  tracking = true; statusLEDMode = STATUS_TRACKING; lastDisplayUpdate = 0;
  displayTrackingScreen(d.rssi, true);
}
void stopTracking() {
  tracking = false; ledsOff(); statusLEDMode = STATUS_READY;
  portENTER_CRITICAL(&trackingMux); newTrackingResult = false; portEXIT_CRITICAL(&trackingMux);
  displayDeviceList();
}

void trackingTask(void *parameter) {
  while (true) {
    if (!tracking) { trackingTaskBusy = false; vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    trackingTaskBusy = true;
    BLEScanResults *results = bleScanner->start(TRACK_SCAN_SECONDS, false);
    bool found = false; int rawRSSI = -100;
    for (int i = 0; i < results->getCount(); i++) {
      BLEAdvertisedDevice d = results->getDevice(i);
      String address = String(d.getAddress().toString().c_str());
      if (address.equalsIgnoreCase(targetAddress)) { rawRSSI = d.getRSSI(); found = true; break; }
    }
    bleScanner->clearResults();
    if (tracking) {
      portENTER_CRITICAL(&trackingMux);
      latestRawRSSI = rawRSSI; latestTargetFound = found; newTrackingResult = true;
      portEXIT_CRITICAL(&trackingMux);
    }
    trackingTaskBusy = false; vTaskDelay(pdMS_TO_TICKS(75));
  }
}

void processTrackingResult() {
  bool ready = false, found = false; int rawRSSI = -100;
  portENTER_CRITICAL(&trackingMux);
  if (newTrackingResult) { ready = true; found = latestTargetFound; rawRSSI = latestRawRSSI; newTrackingResult = false; }
  portEXIT_CRITICAL(&trackingMux);
  if (!ready || !tracking) return;
  if (found) {
    statusLEDMode = STATUS_TRACKING;
    smoothedRSSI = smoothedRSSI <= -99 ? rawRSSI : smoothedRSSI * 0.70 + rawRSSI * 0.30;
    int shown = (int)round(smoothedRSSI);
    addRSSIHistory(shown);
    if (rawRSSI > targetPeakRSSI) targetPeakRSSI = rawRSSI;
    targetObservations++;
    if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
      displayTrackingScreen(shown, true); lastDisplayUpdate = millis();
    }
  } else {
    statusLEDMode = STATUS_TARGET_LOST;
    if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
      displayTrackingScreen(-100, false); lastDisplayUpdate = millis();
    }
  }
}

// ==================== TOUCH ====================
void convertTouchCoordinates(int rawX, int rawY, int &screenX, int &screenY) {
  screenX = constrain(map(rawY, 0, 320, 0, 319), 0, 319);
  screenY = constrain(map(rawX, 170, 0, 0, 169), 0, 169);
}
bool validRawTouch(int x, int y) {
  return x >= RAW_X_MIN && x <= RAW_X_MAX && y >= RAW_Y_MIN && y <= RAW_Y_MAX;
}
void handleDeviceListTap(int x, int y) {
  if (pointInside(x, y, ARROW_X - 3, UP_ARROW_Y, ARROW_WIDTH + 5, UP_ARROW_HEIGHT)) { moveListUp(); return; }
  if (pointInside(x, y, ARROW_X - 3, DOWN_ARROW_Y, ARROW_WIDTH + 5, DOWN_ARROW_HEIGHT)) { moveListDown(); return; }
  if (x < LIST_WIDTH && y >= LIST_TOP && y < BUTTON_TOP) {
    int row = (y - LIST_TOP) / (ROW_HEIGHT + ROW_SPACING);
    int index = firstVisibleDevice + row;
    if (row >= 0 && row < VISIBLE_ROWS && index < deviceCount) {
      if (selectedDevice == index) displayDeviceDetails();
      else { selectedDevice = index; displayDeviceList(); }
    }
    return;
  }
  if (y >= 122) {
    if (x < 90) { scanForDevices(false); displayDeviceList(); return; }
    if (x < 180) { if (deviceCount > 0) { toggleKnownDevice(selectedDevice); displayDeviceList(); } return; }
    if (x < 273 && deviceCount > 0) { startTrackingSelectedDevice(); return; }
  }
}
void handleTrackingTap(int x, int y) {
  if (y < 120) return;
  if (x < 160) { stopTracking(); return; }
  smoothedRSSI = -100;
  tft.fillRect(0, 115, 320, 14, TFT_BLACK); tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COLOR_MUTED, TFT_BLACK); tft.setTextSize(1); tft.drawString("Refreshing...", 160, 121);
  tft.setTextDatum(TL_DATUM);
}
void processTouch() {
  bool pressed = digitalRead(TOUCH_INT) == LOW;
  unsigned long now = millis();
  if (pressed) {
    noteUserActivity();
    if (!touch.read() || touch.getPointNum() == 0) return;
    TP_Point p = touch.getPoint(0); if (!validRawTouch(p.x, p.y)) return;
    int x, y; convertTouchCoordinates(p.x, p.y, x, y);
    if (!touchActive) { touchActive = true; touchStartX = touchCurrentX = x; touchStartY = touchCurrentY = y; }
    else { touchCurrentX = x; touchCurrentY = y; }
    return;
  }
  if (!touchActive) return;
  touchActive = false;
  if (now - lastTouchActionAt < TOUCH_COOLDOWN_MS) return;
  lastTouchActionAt = now;
  int x = (touchStartX + touchCurrentX) / 2;
  int y = (touchStartY + touchCurrentY) / 2;
  noteUserActivity();
  if (currentScreen == SCREEN_DEVICE_LIST) handleDeviceListTap(x, y);
  else if (currentScreen == SCREEN_DEVICE_DETAILS) {
    if (y >= 120) displayDeviceList();
  } else handleTrackingTap(x, y);
}

// ==================== SLEEP / WAKE ====================
void powerDownDevice() {
  tracking = false; statusLEDMode = STATUS_SLEEPING; ledsOff(); digitalWrite(LED_STATUS, LOW);
  drawHeader("POWERING DOWN"); tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.setTextSize(2); tft.drawString("PocketRF sleeping", 160, 67);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(1); tft.drawString("Press either button to wake", 160, 98);
  tft.setTextDatum(TL_DATUM); delay(900);
  while (digitalRead(BUTTON_1) == LOW || digitalRead(BUTTON_2) == LOW) delay(10);
  delay(150); tft.fillScreen(TFT_BLACK); ledsOff(); digitalWrite(LED_STATUS, LOW); delay(100);
  digitalWrite(TFT_POWER, LOW); delay(100);

  rtc_gpio_init((gpio_num_t)BUTTON_1);
  rtc_gpio_set_direction((gpio_num_t)BUTTON_1, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_1); rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_1);
  rtc_gpio_init((gpio_num_t)BUTTON_2);
  rtc_gpio_set_direction((gpio_num_t)BUTTON_2, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_2); rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_2);

  uint64_t wakeMask = (1ULL << BUTTON_1) | (1ULL << BUTTON_2);
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
  delay(50); esp_deep_sleep_start();
}
void checkPowerButtons() {
  bool b1 = digitalRead(BUTTON_1) == LOW;
  bool b2 = digitalRead(BUTTON_2) == LOW;
  if (b1 || b2) noteUserActivity();
  bool both = b1 && b2;
  if (both) {
    if (!powerCombinationActive) { powerCombinationActive = true; powerCombinationStarted = millis(); }
    if (millis() - powerCombinationStarted >= POWER_OFF_HOLD_MS) powerDownDevice();
  } else { powerCombinationActive = false; powerCombinationStarted = 0; }
}

// ==================== SETUP ====================
void setup() {
  rtc_gpio_deinit((gpio_num_t)BUTTON_1); rtc_gpio_deinit((gpio_num_t)BUTTON_2);

  pinMode(LED_1, OUTPUT); pinMode(LED_2, OUTPUT); pinMode(LED_3, OUTPUT); pinMode(LED_STATUS, OUTPUT);
  ledsOff(); digitalWrite(LED_STATUS, LOW);
  statusLEDMode = STATUS_BOOTING;
  xTaskCreatePinnedToCore(statusLEDTask, "StatusLEDTask", 2048, NULL, 1, &statusLEDTaskHandle, 0);

  pinMode(BUTTON_1, INPUT_PULLUP); pinMode(BUTTON_2, INPUT_PULLUP);
  pinMode(TFT_POWER, OUTPUT); digitalWrite(TFT_POWER, HIGH);
  pinMode(TOUCH_INT, INPUT_PULLUP);

  gpio_hold_dis((gpio_num_t)TOUCH_RST);
  pinMode(TOUCH_RST, OUTPUT); digitalWrite(TOUCH_RST, LOW); delay(500);
  digitalWrite(TOUCH_RST, HIGH); delay(500);
  Wire.begin(TOUCH_SDA, TOUCH_SCL); delay(250);
  bool touchStarted = touch.init();

  analogReadResolution(12); analogSetAttenuation(ADC_11db);
  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
  if (!touchStarted) {
    statusLEDMode = STATUS_TARGET_LOST; tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK); tft.setTextSize(2); tft.drawString("TOUCH INIT FAILED", 160, 85);
    while (true) delay(100);
  }

  loadKnownDevices(); readBatteryLevel(); showStartupScreen();
  BLEDevice::init("Matts-C3-PocketRF");
  bleScanner = BLEDevice::getScan();
  bleScanner->setAdvertisedDeviceCallbacks(&scanCallbacks, true); // true = receive duplicate ads/scan responses
  bleScanner->setActiveScan(true);
  bleScanner->setInterval(100);
  bleScanner->setWindow(99);

  xTaskCreatePinnedToCore(trackingTask, "BLETrackingTask", 8192, NULL, 1, &trackingTaskHandle, 0);
  delay(300); scanForDevices(true); displayDeviceList();
  lastUserActivity = millis();
  lastAutoRescan = millis();
}

void loop() {
  checkPowerButtons(); processTouch();
  if (millis() - lastBatteryUpdate >= BATTERY_UPDATE_MS) { readBatteryLevel(); drawBatteryIndicator(); }
  if (tracking) processTrackingResult();

  if (currentScreen == SCREEN_DEVICE_LIST && !screenIsOff &&
      millis() - lastAutoRescan >= AUTO_RESCAN_MS) {
    scanForDevices(false);
    displayDeviceList();
  }

  manageScreenPower();
  delay(5);
}
