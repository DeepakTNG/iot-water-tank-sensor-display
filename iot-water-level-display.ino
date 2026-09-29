// Firmware Version: 0.1.4
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

// --- OLED Screen Configuration (7-Pin SPI Layout) ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define OLED_DC     4   // GPIO4 (NodeMCU Pin: D2)
#define OLED_RESET  5   // GPIO5 (NodeMCU Pin: D1)
#define OLED_CS     15  // GPIO15 (NodeMCU Pin: D8)

// Hardware SPI Pin Maps: GPIO14 -> CLK (D5), GPIO13 -> MOSI (D7)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RESET, OLED_CS);

// --- Configuration Pin & Hold Definition ---
#define PORTAL_BUTTON_PIN 0    // GPIO0 (NodeMCU Pin: D3) - Safe, dedicated input pin with internal pull-up
#define HOLD_TIME_MS      5000

// --- Custom Parameter Containers for WiFiManager ---
char api_endpoint[100] = "https://thesoft.in";
char api_token[100]    = "YOUR_BEARER_TOKEN";
char device_name[50]   = "test_tank";

bool shouldSaveConfig = false;

// --- Application Metrics to Render ---
int water_pct = 0;
float water_liters = 0.0;
float raw_battery_voltage = 0.0;
int battery_pct = 0;
int rssi_signal = 0;
bool api_success = false;

void saveConfigCallback() {
  shouldSaveConfig = true;
}

// WiFiManager AP mode callback to display captive portal information on OLED
void configModeCallback(WiFiManager *myWiFiManager) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("PORTAL ACTIVE");
  
  display.setCursor(0, 14);
  display.println("Connect Wi-Fi to:");
  display.setCursor(0, 26);
  display.println("ESP8266_Config");
  
  display.setCursor(0, 40);
  display.println("Browser URL / IP:");
  display.setTextSize(1);
  display.setCursor(0, 52);
  display.println(WiFi.softAPIP().toString()); // Renders 192.168.4.1
  
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- Starting NodeMCU Water Level Display (v0.1.2) ---");
  
  pinMode(PORTAL_BUTTON_PIN, INPUT_PULLUP);
  
  if(!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("Starting up...");
  display.display();

  // Load custom configs from LittleFS local storage
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
            if (doc.containsKey("api_endpoint")) strcpy(api_endpoint, doc["api_endpoint"]);
            if (doc.containsKey("api_token")) strcpy(api_token, doc["api_token"]);
            if (doc.containsKey("device_name")) strcpy(device_name, doc["device_name"]);
          }
        }
        configFile.close();
      }
    }
  }

  // Ensure WiFi Station mode is set to inspect saved credentials
  WiFi.mode(WIFI_STA);
  String savedSSID = WiFi.SSID();
  Serial.print("Saved WiFi SSID: '");
  Serial.print(savedSSID);
  Serial.println("'");

  // State 1: If NO Wi-Fi credentials are saved, launch Captive Portal immediately
  if (savedSSID.length() == 0) {
    Serial.println("No WiFi credentials found. Launching Captive Portal...");
    startWiFiPortal();
  } else {
    // State 2: Saved credentials exist -> Attempt connection with 15s non-blocking timeout
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
      Serial.println("Connected successfully to Wi-Fi!");
    } else {
      Serial.println("Wi-Fi connection timed out. Proceeding to loop (State 2 Error Screen).");
    }
  }
}

void loop() {
  // State 3: Check manual button press anytime in execution
  checkPortalButton();

  if (WiFi.status() == WL_CONNECTED) {
    updateNetworkSignal();
    fetchTankData();
    drawScreen();
  } else {
    // State 2: Disconnected state shows Wi-Fi Error Screen
    drawWiFiErrorScreen();
  }
  
  // Responsive non-blocking loop delay (~15 seconds update cycle)
  for (int i = 0; i < 150; i++) {
    if (digitalRead(PORTAL_BUTTON_PIN) == LOW) {
      checkPortalButton();
      break; 
    }
    delay(100); 
  }
}

void checkPortalButton() {
  // Check if button is pressed (LOW)
  if (digitalRead(PORTAL_BUTTON_PIN) == LOW) {
    // Debounce check: ensure button stays pressed for at least 50ms to ignore glitches/noise
    delay(50);
    if (digitalRead(PORTAL_BUTTON_PIN) != LOW) {
      return; // False trigger or noise, exit immediately
    }

    unsigned long startTime = millis();
    bool criteriaMet = false;

    // State 3: Block execution and track if button held continuously for 5 seconds
    while (digitalRead(PORTAL_BUTTON_PIN) == LOW) {
      unsigned long elapsed = millis() - startTime;
      long remaining = (HOLD_TIME_MS - elapsed) / 1000 + 1;
      
      if (elapsed >= HOLD_TIME_MS) {
        criteriaMet = true;
        break;
      }
      
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
      
      // Wipe Wi-Fi credentials out of NVS cache as required by State 3
      WiFi.disconnect(true);
      ESP.eraseConfig();
      delay(1000);
      
      startWiFiPortal();
    }
  }
}

void startWiFiPortal() {
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback); 

  WiFiManagerParameter custom_api_endpoint("endpoint", "API Endpoint", api_endpoint, 100);
  WiFiManagerParameter custom_api_token("token", "Bearer Token", api_token, 100);
  WiFiManagerParameter custom_device_name("device", "Device Name", device_name, 50);

  wm.addParameter(&custom_api_endpoint);
  wm.addParameter(&custom_api_token);
  wm.addParameter(&custom_device_name);

  // Requirement 3: AP Hotspot Name exactly 'ESP8266_Config'
  if (!wm.startConfigPortal("ESP8266_Config")) {
    Serial.println("Portal timeout or exited. Restarting ESP...");
    delay(3000);
    ESP.restart();
  }

  // Save updated custom input fields to LittleFS non-volatile flash memory
  if (shouldSaveConfig) {
    strcpy(api_endpoint, custom_api_endpoint.getValue());
    strcpy(api_token, custom_api_token.getValue());
    strcpy(device_name, custom_device_name.getValue());

    DynamicJsonDocument doc(1024);
    doc["api_endpoint"] = api_endpoint;
    doc["api_token"] = api_token;
    doc["device_name"] = device_name;

    File configFile = LittleFS.open("/config.json", "w");
    if (configFile) {
      serializeJson(doc, configFile);
      configFile.close();
      Serial.println("Custom configuration parameters saved to LittleFS.");
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

void updateNetworkSignal() {
  long rssi = WiFi.RSSI();
  if (rssi > -50) rssi_signal = 4;
  else if (rssi > -60) rssi_signal = 3;
  else if (rssi > -70) rssi_signal = 2;
  else if (rssi > -80) rssi_signal = 1;
  else rssi_signal = 0;
}

void fetchTankData() {
  WiFiClientSecure client;
  client.setInsecure(); // Non-blocking HTTPS request with insecure footprint

  HTTPClient http;
  String url = String(api_endpoint) + "/api/devices/" + String(device_name) + "/latest";
  
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  String authHeader = "Bearer " + String(api_token);
  http.addHeader("Authorization", authHeader);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonObject data_payload = doc["data"]["payload"];
      water_pct = data_payload["water_percentage"];
      water_liters = data_payload["available_water_liters"];
      raw_battery_voltage = data_payload["battery_voltage"];
      
      // Calculate battery percentage: Constrain 18650 cell between 3.2V (0%) and 4.2V (100%)
      float constrained_v = constrain(raw_battery_voltage, 3.2, 4.2);
      battery_pct = (int)(((constrained_v - 3.2) / 1.0) * 100.0);
      api_success = true; // API call and JSON parse succeeded
    } else {
      api_success = false; // JSON parse failed
      Serial.println("API JSON parse error.");
    }
  } else {
    api_success = false; // HTTP call failed or non-200 response
    Serial.print("API HTTP error code: ");
    Serial.println(httpCode);
  }
  http.end();
}

// Draw WiFi error 'X' icon in status bar area (X=0, Y=0 region)
void drawWiFiErrorIcon(int xOffset) {
  // Draw a small 'X' using two diagonal lines in a 9x9 box
  display.drawLine(xOffset,     2, xOffset + 8, 10, SSD1306_WHITE);
  display.drawLine(xOffset + 8, 2, xOffset,     10, SSD1306_WHITE);
}

// Draw API status icon: checkmark on success, '!' exclamation on failure
void drawApiStatusIcon(int xOffset) {
  if (api_success) {
    // Tiny checkmark: angled tick shape
    display.drawLine(xOffset,     6, xOffset + 2, 9,  SSD1306_WHITE);
    display.drawLine(xOffset + 2, 9, xOffset + 6, 3,  SSD1306_WHITE);
  } else {
    // Exclamation '!': vertical bar + dot
    display.drawLine(xOffset + 3, 2, xOffset + 3, 7, SSD1306_WHITE);
    display.drawPixel(xOffset + 3, 10, SSD1306_WHITE);
  }
}

void drawWiFiErrorScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Status bar: show WiFi error X icon in top-left
  drawWiFiErrorIcon(2);

  // API icon also shows failure when WiFi is down
  drawApiStatusIcon(18);

  // Separator line
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

  // --- TOP STATUS BAR LAYER (Y=0 to Y=12) ---
  // Left Aligned: WiFi signal bars OR error X icon if disconnected
  if (WiFi.status() == WL_CONNECTED) {
    // Draw standard 4-bar RSSI signal strength meter
    for (int i = 0; i < 4; i++) {
      int barHeight = (i + 1) * 2;
      int xPos = 2 + (i * 3);
      int yPos = 10 - barHeight;
      if (i < rssi_signal) {
        display.fillRect(xPos, yPos, 2, barHeight, SSD1306_WHITE);
      } else {
        display.drawRect(xPos, yPos, 2, barHeight, SSD1306_WHITE);
      }
    }
  } else {
    // WiFi disconnected: draw error X icon instead of signal bars
    drawWiFiErrorIcon(2);
  }

  // Center: API status icon (checkmark = success, exclamation = failure)
  // Positioned just to the right of the WiFi signal bars area
  drawApiStatusIcon(18);

  // Right Aligned: Drawn battery icon outline with inner filled block relative to battery %
  display.drawRect(104, 2, 18, 8, SSD1306_WHITE);
  display.fillRect(122, 4, 2, 4, SSD1306_WHITE); // Battery terminal nub
  int innerWidth = map(constrain(battery_pct, 0, 100), 0, 100, 0, 14);
  display.fillRect(106, 4, innerWidth, 4, SSD1306_WHITE);

  // Separator: Crisp horizontal dividing line at Y=12
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

  // --- MAIN BODY AREA ---
  // Title: "WATER TANK" text
  display.setTextSize(1);
  display.setCursor(0, 18);
  display.print("WATER TANK");

  // Large Font: Current water percentage value followed by "%" sign
  display.setTextSize(3);
  display.setCursor(0, 32);
  display.print(water_pct);
  display.setTextSize(1);
  display.print(" %");

  // Right Side Align: Available water volume (1 decimal place) with "Liters" directly underneath
  display.setTextSize(1);
  display.setCursor(75, 34);
  display.print(water_liters, 1);
  display.setCursor(75, 46);
  display.print(" Liters");

  display.display();
}
