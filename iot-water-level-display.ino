// Firmware Version: 0.1.6
// NodeMCU ESP8266 IoT Water Level Display Firmware

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>

// --- OLED Screen Configuration (7-Pin SPI Layout) ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define OLED_DC     4   // GPIO4 (NodeMCU Pin: D2)
#define OLED_RESET  5   // GPIO5 (NodeMCU Pin: D1)
#define OLED_CS     15  // GPIO15 (NodeMCU Pin: D8)

// Hardware SPI Pin Maps: GPIO14 -> CLK (D5), GPIO13 -> MOSI (D7)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RESET, OLED_CS);

// --- Configuration Pin & Hold Definition ---
#define PORTAL_BUTTON_PIN 0    // GPIO0 (NodeMCU Pin: D3) - Safe, isolated from SPI bus
#define HOLD_TIME_MS      5000

// --- A0 Voltage Divider Constants (22kΩ bottom, 100kΩ top) ---
// V_real = ADC * (3.3 / 1023.0) * ((100000 + 22000) / 22000)
#define VDIV_RATIO        5.5454f  // (100k + 22k) / 22k
#define ADC_REF_V         3.3f
#define ADC_MAX           1023.0f

// --- OTA Web Update Server ---
ESP8266WebServer otaServer(8080);
ESP8266HTTPUpdateServer httpUpdater;

// --- Custom Parameter Containers for WiFiManager ---
char api_endpoint[100]        = "https://thesoft.in";
char api_token[100]           = "YOUR_BEARER_TOKEN";
char device_name[50]          = "test_tank";
char display_timeout_sec[4]   = "30";  // Display off timeout in seconds (configurable via portal)
char api_interval_sec[5]      = "300"; // API fetch interval in seconds (default 5 minutes)

bool shouldSaveConfig = false;

// --- Application Metrics ---
int   water_pct            = 0;
float water_liters         = 0.0;
float raw_battery_voltage  = 0.0;   // Sensor battery from API
int   battery_pct          = 0;     // Sensor battery %
float esp_battery_voltage  = 0.0;   // ESP local battery via A0
int   esp_battery_pct      = 0;     // ESP local battery %
int   rssi_signal          = 0;
bool  api_success          = false;

// --- Display Sleep State ---
bool          displayOn        = true;
unsigned long lastDisplayWake  = 0;

// --- API Fetch Timing ---
unsigned long lastApiCall      = 0;  // Timestamp of last successful fetch cycle

// -------------------------------------------------------
// HELPERS
// -------------------------------------------------------

String getAPName() {
  uint32_t chipId = ESP.getChipId();
  char apName[20];
  snprintf(apName, sizeof(apName), "Tank_%06X", chipId & 0xFFFFFF);
  return String(apName);
}

void wakeDisplay() {
  if (!displayOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    displayOn = true;
  }
  lastDisplayWake = millis();
}

void readESPBattery() {
  int raw = analogRead(A0);
  esp_battery_voltage = (raw / ADC_MAX) * ADC_REF_V * VDIV_RATIO;
  // Map 18650 range: 3.2V = 0%, 4.2V = 100%
  float constrained_v = constrain(esp_battery_voltage, 3.2, 4.2);
  esp_battery_pct = (int)(((constrained_v - 3.2) / 1.0) * 100.0);
}

// -------------------------------------------------------
// CALLBACKS
// -------------------------------------------------------

void saveConfigCallback() {
  shouldSaveConfig = true;
}

void configModeCallback(WiFiManager *myWiFiManager) {
  wakeDisplay();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("PORTAL ACTIVE");

  display.setCursor(0, 12);
  display.println("Connect Wi-Fi to:");
  display.setCursor(0, 22);
  display.println(myWiFiManager->getConfigPortalSSID()); // Dynamic AP name

  display.setCursor(0, 36);
  display.println("Browser URL / IP:");
  display.setCursor(0, 48);
  display.println(WiFi.softAPIP().toString());

  display.display();
}

// -------------------------------------------------------
// SETUP
// -------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- NodeMCU Water Level Display v0.1.5 ---");

  pinMode(PORTAL_BUTTON_PIN, INPUT_PULLUP);

  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("Starting up...");
  display.display();
  lastDisplayWake = millis();

  // Load config from LittleFS
  if (LittleFS.begin()) {
    if (LittleFS.exists("/config.json")) {
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        if (size > 0 && size < 1024) {
          std::unique_ptr<char[]> buf(new char[size + 1]);
          configFile.readBytes(buf.get(), size);
          buf[size] = '\0';
          DynamicJsonDocument doc(1024);
          if (deserializeJson(doc, buf.get()) == DeserializationError::Ok) {
            if (doc.containsKey("api_endpoint"))     strcpy(api_endpoint,       doc["api_endpoint"]);
            if (doc.containsKey("api_token"))        strcpy(api_token,          doc["api_token"]);
            if (doc.containsKey("device_name"))      strcpy(device_name,        doc["device_name"]);
            if (doc.containsKey("display_timeout"))  strcpy(display_timeout_sec, doc["display_timeout"]);
            if (doc.containsKey("api_interval"))     strcpy(api_interval_sec,   doc["api_interval"]);
          }
        }
        configFile.close();
      }
    }
  }

  WiFi.mode(WIFI_STA);
  String savedSSID = WiFi.SSID();
  Serial.print("Saved WiFi SSID: '");
  Serial.print(savedSSID);
  Serial.println("'");

  if (savedSSID.length() == 0) {
    Serial.println("No WiFi credentials found. Launching Captive Portal...");
    startWiFiPortal();
  } else {
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Connecting Wi-Fi...");
    display.display();

    WiFi.begin();
    unsigned long startConnect = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startConnect < 15000) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to Wi-Fi!");
      // Start mDNS and OTA HTTP update server
      if (MDNS.begin("tank-display")) {
        Serial.println("mDNS started: http://tank-display.local:8080/update");
      }
      httpUpdater.setup(&otaServer, "/update");
      otaServer.begin();
      Serial.println("OTA update server started on port 8080.");
    } else {
      Serial.println("Wi-Fi connection timed out. Entering loop.");
    }
  }
}

// -------------------------------------------------------
// MAIN LOOP
// -------------------------------------------------------

void loop() {
  // Handle OTA update server requests
  if (WiFi.status() == WL_CONNECTED) {
    otaServer.handleClient();
    MDNS.update();
  }

  checkPortalButton();

  unsigned long nowMs           = millis();
  unsigned long apiIntervalMs   = (unsigned long)atoi(api_interval_sec) * 1000UL;
  bool          apiFetchDue     = (nowMs - lastApiCall >= apiIntervalMs);

  if (WiFi.status() == WL_CONNECTED) {
    updateNetworkSignal();
    readESPBattery();

    // Only call the API when the configured interval has elapsed
    if (apiFetchDue) {
      fetchTankData();
      lastApiCall = millis();
      Serial.print("API fetched. Next in ");
      Serial.print(api_interval_sec);
      Serial.println(" sec.");

      // Wake display on each API fetch cycle (feature 4)
      wakeDisplay();
    }

    drawScreen();
  } else {
    wakeDisplay();
    drawWiFiErrorScreen();
  }

  // Check if display should sleep (configurable timeout)
  unsigned long timeoutMs = (unsigned long)atoi(display_timeout_sec) * 1000UL;
  if (displayOn && timeoutMs > 0 && (millis() - lastDisplayWake >= timeoutMs)) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displayOn = false;
    Serial.println("Display turned off (timeout).");
  }

  // 1-second non-blocking tick — keeps OTA server and button responsive across long intervals
  for (int i = 0; i < 10; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      otaServer.handleClient();
      MDNS.update();
    }
    if (digitalRead(PORTAL_BUTTON_PIN) == LOW) {
      checkPortalButton();
      break;
    }
    delay(100);
  }
}

// -------------------------------------------------------
// BUTTON HANDLER
// -------------------------------------------------------

void checkPortalButton() {
  if (digitalRead(PORTAL_BUTTON_PIN) == LOW) {
    // Debounce: require 50ms sustained press
    delay(50);
    if (digitalRead(PORTAL_BUTTON_PIN) != LOW) {
      return;
    }

    // Feature 5: Wake display on any valid button press
    wakeDisplay();

    unsigned long startTime = millis();
    bool criteriaMet = false;

    while (digitalRead(PORTAL_BUTTON_PIN) == LOW) {
      unsigned long elapsed = millis() - startTime;

      if (elapsed >= HOLD_TIME_MS) {
        criteriaMet = true;
        break;
      }

      long remaining = (HOLD_TIME_MS - elapsed) / 1000 + 1;
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0, 10);
      display.println("Keep holding button");
      display.println("to reset Wi-Fi:");
      display.setTextSize(3);
      display.setCursor(50, 38);
      display.print(remaining);
      display.display();
      delay(50);
    }

    if (criteriaMet) {
      display.clearDisplay();
      display.setCursor(0, 20);
      display.setTextSize(1);
      display.println("Wiping Wi-Fi...");
      display.println("Launching Portal...");
      display.display();

      WiFi.disconnect(true);
      ESP.eraseConfig();
      delay(1000);

      startWiFiPortal();
    }
  }
}

// -------------------------------------------------------
// WiFi PORTAL
// -------------------------------------------------------

void startWiFiPortal() {
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);

  WiFiManagerParameter custom_api_endpoint("endpoint",  "API Endpoint",            api_endpoint,        100);
  WiFiManagerParameter custom_api_token("token",        "Bearer Token",            api_token,           100);
  WiFiManagerParameter custom_device_name("device",     "Device Name",             device_name,          50);
  WiFiManagerParameter custom_timeout("timeout",        "Display Timeout (sec)",   display_timeout_sec,   4);
  WiFiManagerParameter custom_interval("interval",      "API Fetch Interval (sec)", api_interval_sec,     5);

  wm.addParameter(&custom_api_endpoint);
  wm.addParameter(&custom_api_token);
  wm.addParameter(&custom_device_name);
  wm.addParameter(&custom_timeout);
  wm.addParameter(&custom_interval);

  // Feature 6: Dynamic AP name Tank_<ChipID>
  String apName = getAPName();
  Serial.print("Starting portal with AP: ");
  Serial.println(apName);

  if (!wm.startConfigPortal(apName.c_str())) {
    Serial.println("Portal timeout. Restarting...");
    delay(3000);
    ESP.restart();
  }

  if (shouldSaveConfig) {
    strcpy(api_endpoint,        custom_api_endpoint.getValue());
    strcpy(api_token,           custom_api_token.getValue());
    strcpy(device_name,         custom_device_name.getValue());
    strcpy(display_timeout_sec, custom_timeout.getValue());
    strcpy(api_interval_sec,    custom_interval.getValue());

    DynamicJsonDocument doc(1024);
    doc["api_endpoint"]    = api_endpoint;
    doc["api_token"]       = api_token;
    doc["device_name"]     = device_name;
    doc["display_timeout"] = display_timeout_sec;
    doc["api_interval"]    = api_interval_sec;

    File configFile = LittleFS.open("/config.json", "w");
    if (configFile) {
      serializeJson(doc, configFile);
      configFile.close();
      Serial.println("Configuration saved to LittleFS.");
    }
    shouldSaveConfig = false;

    display.clearDisplay();
    display.setCursor(0, 20);
    display.setTextSize(1);
    display.println("Config Saved!");
    display.println("Restarting...");
    display.display();
    delay(2000);
    ESP.restart();
  }

  display.clearDisplay();
}

// -------------------------------------------------------
// NETWORK SIGNAL
// -------------------------------------------------------

void updateNetworkSignal() {
  long rssi = WiFi.RSSI();
  if      (rssi > -50) rssi_signal = 4;
  else if (rssi > -60) rssi_signal = 3;
  else if (rssi > -70) rssi_signal = 2;
  else if (rssi > -80) rssi_signal = 1;
  else                 rssi_signal = 0;
}

// -------------------------------------------------------
// API FETCH
// -------------------------------------------------------

void fetchTankData() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(api_endpoint) + "/api/devices/" + String(device_name) + "/latest";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(api_token));

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonObject data_payload   = doc["data"]["payload"];
      water_pct                 = data_payload["water_percentage"];
      water_liters              = data_payload["available_water_liters"];
      raw_battery_voltage       = data_payload["battery_voltage"];

      // 18650 sensor battery: 3.2V = 0%, 4.2V = 100%
      float cv = constrain(raw_battery_voltage, 3.2, 4.2);
      battery_pct = (int)(((cv - 3.2) / 1.0) * 100.0);
      api_success = true;
    } else {
      api_success = false;
      Serial.println("API JSON parse error.");
    }
  } else {
    api_success = false;
    Serial.print("API HTTP error: ");
    Serial.println(httpCode);
  }
  http.end();
}

// -------------------------------------------------------
// DRAWING HELPERS
// -------------------------------------------------------

// WiFi error X icon (9x9 px area)
void drawWiFiErrorIcon(int x) {
  display.drawLine(x,     2, x + 8, 10, SSD1306_WHITE);
  display.drawLine(x + 8, 2, x,     10, SSD1306_WHITE);
}

// API status icon: checkmark or exclamation (7px wide)
void drawApiStatusIcon(int x) {
  if (api_success) {
    display.drawLine(x,     6, x + 2, 9, SSD1306_WHITE);
    display.drawLine(x + 2, 9, x + 6, 3, SSD1306_WHITE);
  } else {
    display.drawLine(x + 3, 2, x + 3, 7, SSD1306_WHITE);
    display.drawPixel(x + 3, 10, SSD1306_WHITE);
  }
}

// Battery icon: 16x8 outline + 2px nub + filled inner bar
// xOffset: left edge of the 18px icon block
void drawBatteryIcon(int x, int pct) {
  display.drawRect(x, 2, 16, 8, SSD1306_WHITE);
  display.fillRect(x + 16, 4, 2, 4, SSD1306_WHITE);           // terminal nub
  int fill = map(constrain(pct, 0, 100), 0, 100, 0, 12);
  display.fillRect(x + 2, 4, fill, 4, SSD1306_WHITE);

  // Feature 2: Low battery warning — draw '!' above icon if < 5%
  if (pct < 5) {
    display.drawLine(x + 7, 0, x + 7, 0, SSD1306_WHITE);      // just a dot at top (tight space)
    // Draw an inverted small '!' character clipped into 1px above icon
    display.drawPixel(x + 8, 0, SSD1306_WHITE);
    display.drawPixel(x + 9, 0, SSD1306_WHITE);
  }
}

// -------------------------------------------------------
// SCREENS
// -------------------------------------------------------

void drawWiFiErrorScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Status bar
  drawWiFiErrorIcon(2);
  drawApiStatusIcon(18);
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 16);
  display.println("Wi-Fi Error!");
  display.println("Failed to connect.");
  display.println("Check your router.");
  display.println("");
  display.println("Hold btn 5s to reset.");

  display.display();
}

void drawScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- STATUS BAR (Y=0 to Y=11) ---
  // Screen = 128px wide. Layout right-to-left:
  //   [Sensor Batt icon 16px + nub 2px] at X=110  => ends at X=128
  //   [ESP Batt icon 16px + nub 2px]    at X=88   => ends at X=106 (2px gap before sensor)
  //   'S' label (6px)                   at X=104  => just before sensor battery
  //   'E' label (6px)                   at X=82   => just before ESP battery
  //   [API icon]  at X=18
  //   [WiFi bars] at X=2

  // [X=2] WiFi signal bars OR error X icon
  if (WiFi.status() == WL_CONNECTED) {
    for (int i = 0; i < 4; i++) {
      int bh = (i + 1) * 2;
      int bx = 2 + (i * 3);
      int by = 10 - bh;
      if (i < rssi_signal) display.fillRect(bx, by, 2, bh, SSD1306_WHITE);
      else                  display.drawRect(bx, by, 2, bh, SSD1306_WHITE);
    }
  } else {
    drawWiFiErrorIcon(2);
  }

  // [X=18] API status icon (✓ success or ! failure)
  drawApiStatusIcon(18);

  // Right-aligned battery icons with clear gap between them:
  //   Sensor battery (rightmost):  icon at X=108, ends X=126, nub X=126
  //   ESP battery (second right):  icon at X=82,  ends X=100, nub X=100
  //   Gap between them: 6px (X=101 to X=107)
  //   'E' label 6px before ESP icon: X=76
  //   'S' label 6px before sensor icon: X=102

  display.setTextSize(1);
  display.setCursor(76, 2);   // 'E' label — ESP/display battery
  display.print("E");
  display.setCursor(102, 2);  // 'S' label — sensor battery
  display.print("S");

  // [X=82]  ESP local battery icon (A0 voltage divider) — second from right
  drawBatteryIcon(82, esp_battery_pct);

  // [X=108] Sensor battery icon (from API) — extreme right
  drawBatteryIcon(108, battery_pct);

  // Separator line
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

  // --- MAIN BODY ---
  display.setTextSize(1);
  display.setCursor(0, 18);
  display.print("WATER TANK");

  display.setTextSize(3);
  display.setCursor(0, 32);
  display.print(water_pct);
  display.setTextSize(1);
  display.print(" %");

  display.setTextSize(1);
  display.setCursor(75, 34);
  display.print(water_liters, 1);
  display.setCursor(75, 46);
  display.print(" Liters");

  display.display();
}

