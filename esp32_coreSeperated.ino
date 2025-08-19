#include <SPI.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <DHT.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// =============================================================================
// Hardware Configuration
// =============================================================================
#define ETH_CS_PIN       15
#define ETH_RST_PIN      5
#define ETH_SCK_PIN      14
#define ETH_MISO_PIN     12
#define ETH_MOSI_PIN     13
#define DHTPIN           4
#define DHTTYPE          DHT22
#define BUTTON_PIN       25       // Button connected to GPIO 25
#define LCD_ADDRESS      0x27     // I2C address for LCD
#define LCD_COLS         16       // LCD columns
#define LCD_ROWS         2        // LCD rows
#define BUZZER_PIN       26       // Active buzzer
#define LED_PIN          27       // Status LED
#define RELAY_PIN        32       // Relay control
#define RELAY_ACTIVE_MODE LOW     // LOW for active-low, HIGH for active-high

// =============================================================================
// Network Credentials
// =============================================================================
const char* WIFI_SSID = "iran-smarttech";
const char* WIFI_PASS = "Hh3040650051@";
const char* AP_SSID   = "ESP32-SENSOR-AP";
const char* AP_PASS   = "password123";
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};

// =============================================================================
// Type Definitions
// =============================================================================
enum DisplayMode { 
    SENSOR_MODE, 
    IP_MODE 
};

enum AlarmPatternState {
    ALARM_OFF,          // No alarm
    ALARM_SHORT1_ON,    // First short beep (100ms)
    ALARM_SHORT1_OFF,   // First short pause (100ms)
    ALARM_SHORT2_ON,    // Second short beep (100ms)
    ALARM_SHORT2_OFF,   // Second short pause (100ms)
    ALARM_SHORT3_ON,    // Third short beep (100ms)
    ALARM_SHORT3_OFF,   // Third short pause (100ms)
    ALARM_LONG_ON,      // Long beep (300ms)
    ALARM_LONG_OFF      // Long pause (300ms)
};

struct SharedData {
    float temperature;
    float humidity;
    char ipAddress[16];  // Store IP as string (xxx.xxx.xxx.xxx)
};

struct ThresholdSettings {
    float temp_high;
    float temp_low;
    float humid_high;
    float humid_low;
    bool buzzer_enabled = true;  // Buzzer enable/disable
    bool led_enabled = true;     // LED enable/disable
    bool relay_enabled = true;   // Relay enable/disable
    int relay_delay = 5;         // Relay turn-off delay in seconds
};

struct AuthSettings {
    String username;
    String password;
};

// =============================================================================
// Global Objects
// =============================================================================
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
Preferences preferences;
EthernetServer ethServer(80);
WiFiServer wifiServer(80);

// =============================================================================
// Global Variables
// =============================================================================
SharedData sensorData;
SemaphoreHandle_t dataMutex;

ThresholdSettings thresholds = {30.0, 15.0, 70.0, 30.0};  // Default values
AuthSettings auth = {"admin", "password"};                 // Default credentials

volatile bool ethActive = false;
volatile bool wifiActive = false;
volatile bool apActive = false;

volatile bool alertActive = false;           // Global alert status
AlarmPatternState alarmState = ALARM_OFF;    // Current alarm pattern state
unsigned long lastAlarmChange = 0;           // Last alarm state change time
unsigned long relayDeactivateTime = 0;       // Scheduled relay deactivation time
bool relayActive = false;                    // Current relay state

// =============================================================================
// LCD Display Manager Class
// =============================================================================
class LCDManager {
private:
    DisplayMode currentMode = SENSOR_MODE;
    DisplayMode lastMode = SENSOR_MODE;
    float lastTemp = -100.0;
    float lastHumi = -100.0;
    char lastIP[16] = "";
    char lastAlertLine0[3] = "";
    char lastAlertLine1[3] = "";
    char lastNetworkStatus[3] = "";

public:
    void init() {
        lcd.init();
        lcd.backlight();
        lcd.clear();
    }

    void update(DisplayMode mode, float temp, float humi, const char* ip, 
                bool ethActive, bool wifiActive, bool apActive,
                bool tempAlert, bool humiAlert) {
                
        // Handle mode changes
        if (mode != lastMode) {
            lcd.clear();
            lastMode = mode;
            
            if (mode == SENSOR_MODE) {
                // Setup static parts
                lcd.setCursor(0, 0);
                lcd.print("Temp: ");
                lcd.setCursor(0, 1);
                lcd.print("Hum:  ");
            }
        }
        currentMode = mode;

        if (mode == SENSOR_MODE) {
            updateSensorMode(temp, humi, ethActive, wifiActive, apActive, tempAlert, humiAlert);
        } 
        else if (mode == IP_MODE) {
            updateIPMode(ip);
        }
    }

private:
    void updateSensorMode(float temp, float humi, bool ethActive, bool wifiActive, bool apActive,
                          bool tempAlert, bool humiAlert) {
        // Update temperature if changed
        if (abs(temp - lastTemp) > 0.09) {
            lcd.setCursor(6, 0);
            lcd.print(temp, 1);
            lcd.print("C ");
            lastTemp = temp;
        }

        // Update humidity if changed
        if (abs(humi - lastHumi) > 0.09) {
            lcd.setCursor(6, 1);
            lcd.print(humi, 1);
            lcd.print("% ");
            lastHumi = humi;
        }

        // Determine alert status
        char currentAlertLine0[3] = "";
        char currentAlertLine1[3] = "";
        char currentNetworkStatus[3] = "";
        
        if (tempAlert) {
            if (lastTemp > thresholds.temp_high) 
                strcpy(currentAlertLine0, "TH");
            else 
                strcpy(currentAlertLine0, "TL");
        }
        
        if (humiAlert) {
            if (lastHumi > thresholds.humid_high) 
                strcpy(currentAlertLine1, "HH");
            else 
                strcpy(currentAlertLine1, "HL");
        }
        
        if (!tempAlert && !humiAlert) {
            if (ethActive) 
                strcpy(currentNetworkStatus, "ET");
            else if (wifiActive) 
                strcpy(currentNetworkStatus, "WF");
            else if (apActive) 
                strcpy(currentNetworkStatus, "AP");
        }

        // Update alerts and status if changed
        if (strcmp(currentAlertLine0, lastAlertLine0) != 0) {
            lcd.setCursor(13, 0);
            lcd.print(currentAlertLine0);
            strcpy(lastAlertLine0, currentAlertLine0);
        }
        
        if (strcmp(currentAlertLine1, lastAlertLine1) != 0) {
            lcd.setCursor(13, 1);
            lcd.print(currentAlertLine1);
            strcpy(lastAlertLine1, currentAlertLine1);
        }
        
        if (strcmp(currentNetworkStatus, lastNetworkStatus) != 0) {
            lcd.setCursor(12, 0);
            lcd.print(currentNetworkStatus);
            strcpy(lastNetworkStatus, currentNetworkStatus);
        }
    }

    void updateIPMode(const char* ip) {
        if (strcmp(ip, lastIP) != 0) {
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("IP Address:");
            lcd.setCursor(0, 1);
            lcd.print(ip);
            strcpy(lastIP, ip);
        }
    }
};

LCDManager lcdManager;

// =============================================================================
// Core Task Functions
// =============================================================================
void ethernetTask(void *pvParameters) {
    Serial.println("[C0] Starting Ethernet...");
    
    // W5500 Hardware Reset Sequence
    pinMode(ETH_RST_PIN, OUTPUT);
    digitalWrite(ETH_RST_PIN, HIGH);
    delay(100);
    digitalWrite(ETH_RST_PIN, LOW);
    delay(100);
    digitalWrite(ETH_RST_PIN, HIGH);
    delay(500);
    
    // Initialize SPI and Ethernet
    SPI.begin(ETH_SCK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
    Ethernet.init(ETH_CS_PIN);
    
    // Attempt Ethernet connection with DHCP
    if (Ethernet.begin(mac, 10000)) {
        Serial.print("[C0] Ethernet connected! IP: ");
        Serial.println(Ethernet.localIP());
        
        // Update IP in shared data
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            IPAddress ip = Ethernet.localIP();
            snprintf(sensorData.ipAddress, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
            xSemaphoreGive(dataMutex);
        }
        
        // Disable WiFi
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        
        ethActive = true;
        ethServer.begin();
        
        // Main Ethernet processing loop
        while (true) {
            EthernetClient client = ethServer.available();
            if (client) {
                handleHTTPRequest(client);
                client.stop();
            }
            Ethernet.maintain();
            vTaskDelay(1);
        }
    } else {
        Serial.println("[C0] Ethernet connection failed");
    }
    vTaskDelete(NULL);
}

void wifiSensorTask(void *pvParameters) {
    Serial.println("[C1] Starting sensor and network fallback...");
    
    // Initialize hardware
    DHT dht(DHTPIN, DHTTYPE);
    dht.begin();
    pinMode(BUTTON_PIN, INPUT_PULLUP);  // Button with internal pull-up
    
    // Initialize alarm components
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    digitalWrite(RELAY_PIN, !RELAY_ACTIVE_MODE);  // Start in inactive state

    // Initialize LCD
    Wire.begin();
    lcdManager.init();
    lcd.setCursor(0, 0);
    lcd.print("System Starting");
    
    // Wait for Ethernet outcome
    vTaskDelay(11000 / portTICK_PERIOD_MS);
    
    // Activate WiFi if Ethernet failed
    if (!ethActive) {
        Serial.println("[C1] Starting WiFi...");
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        
        uint8_t attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            Serial.print(".");
            attempts++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.print("\n[C1] WiFi connected! IP: ");
            Serial.println(WiFi.localIP());
            updateIPAddress(WiFi.localIP());
            wifiActive = true;
            wifiServer.begin();
        } else {
            Serial.println("\n[C1] WiFi failed! Starting AP...");
            WiFi.softAP(AP_SSID, AP_PASS);
            Serial.print("[C1] AP IP: ");
            Serial.println(WiFi.softAPIP());
            updateIPAddress(WiFi.softAPIP());
            apActive = true;
            wifiServer.begin();
        }
    }
    
    // Main sensor and LCD loop
    unsigned long lastSensorTime = 0;
    unsigned long lastLCDUpdate = 0;
    DisplayMode displayMode = SENSOR_MODE;
    unsigned long buttonPressStart = 0;
    unsigned long ipDisplayStart = 0;
    bool buttonActive = false;
    bool lastAlertCondition = false;
    
    while (true) {
        unsigned long currentTime = millis();
        
        // Read sensor every 2 seconds
        if (currentTime - lastSensorTime >= 2000) {
            float temp = dht.readTemperature();
            float humi = dht.readHumidity();
            
            if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
                if (!isnan(temp)) sensorData.temperature = temp;
                if (!isnan(humi)) sensorData.humidity = humi;
                xSemaphoreGive(dataMutex);
            }
            lastSensorTime = currentTime;
        }
        
        // Handle button press
        if (digitalRead(BUTTON_PIN) == LOW) {  // Button pressed (active-low)
            if (!buttonActive) {
                buttonActive = true;
                buttonPressStart = currentTime;
            }
            
            // Check for 5-second hold
            if (buttonActive && (currentTime - buttonPressStart >= 5000)) {
                displayMode = IP_MODE;
                ipDisplayStart = currentTime;
            }
        } else {
            if (buttonActive) {
                buttonActive = false;
            }
        }
        
        // Return to sensor mode after 5 seconds in IP mode
        if (displayMode == IP_MODE && (currentTime - ipDisplayStart >= 5000)) {
            displayMode = SENSOR_MODE;
        }
        
        // Check for alert condition
        bool tempAlert = false, humiAlert = false;
        bool alertCondition = false;
        
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            tempAlert = (sensorData.temperature > thresholds.temp_high) || 
                       (sensorData.temperature < thresholds.temp_low);
            humiAlert = (sensorData.humidity > thresholds.humid_high) || 
                       (sensorData.humidity < thresholds.humid_low);
            alertCondition = tempAlert || humiAlert;
            xSemaphoreGive(dataMutex);
        }
        
        // Update alert status only when it changes
        if (alertCondition != lastAlertCondition) {
            alertActive = alertCondition;
            lastAlertCondition = alertCondition;
            
            if (!alertCondition) {
                alarmState = ALARM_OFF;  // Reset alarm pattern
            }
        }
        
        // Update alarm pattern
        if (alertActive) {
            updateAlarmPattern();
        } else {
            // Ensure outputs are off when not in alert
            digitalWrite(BUZZER_PIN, LOW);
            digitalWrite(LED_PIN, LOW);
        }
        
        // Manage relay state
        manageRelay(alertCondition);
        
        // Update LCD every 500ms
        if (currentTime - lastLCDUpdate >= 500) {
            // Get sensor data safely
            float temp, humi;
            bool tempAlert = false, humiAlert = false;
            char ip[16];
            
            if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
                temp = sensorData.temperature;
                humi = sensorData.humidity;
                strcpy(ip, sensorData.ipAddress);
                
                // Check for alerts
                tempAlert = (temp > thresholds.temp_high) || (temp < thresholds.temp_low);
                humiAlert = (humi > thresholds.humid_high) || (humi < thresholds.humid_low);
                
                xSemaphoreGive(dataMutex);
            }
            
            // Update LCD efficiently
            lcdManager.update(
                displayMode, 
                temp, 
                humi, 
                ip,
                ethActive,
                wifiActive,
                apActive,
                tempAlert,
                humiAlert
            );
            
            lastLCDUpdate = currentTime;
        }
        
        // Handle WiFi/AP clients
        if (!ethActive) {
            WiFiClient client = wifiServer.available();
            if (client) {
                handleHTTPRequest(client);
                client.stop();
            }
        }

        vTaskDelay(10);
    }
}

// =============================================================================
// Hardware Management Functions
// =============================================================================
void updateIPAddress(IPAddress ip) {
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        snprintf(sensorData.ipAddress, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        xSemaphoreGive(dataMutex);
    }
}

void updateAlarmPattern() {
    unsigned long currentTime = millis();
    
    switch (alarmState) {
        case ALARM_OFF:
            if (alertActive) {
                alarmState = ALARM_SHORT1_ON;
                lastAlarmChange = currentTime;
            }
            break;
            
        case ALARM_SHORT1_ON:
            if (currentTime - lastAlarmChange >= 100) {
                alarmState = ALARM_SHORT1_OFF;
                lastAlarmChange = currentTime;
            }
            break;
            
        case ALARM_SHORT1_OFF:
            if (currentTime - lastAlarmChange >= 100) {
                alarmState = ALARM_SHORT2_ON;
                lastAlarmChange = currentTime;
            }
            break;
            
        case ALARM_SHORT2_ON:
            if (currentTime - lastAlarmChange >= 100) {
                alarmState = ALARM_SHORT2_OFF;
                lastAlarmChange = currentTime;
            }
            break;
            
        case ALARM_SHORT2_OFF:
            if (currentTime - lastAlarmChange >= 100) {
                alarmState = ALARM_SHORT3_ON;
                lastAlarmChange = currentTime;
            }
            break;
            
        case ALARM_SHORT3_ON:
            if (currentTime - lastAlarmChange >= 100) {
                alarmState = ALARM_SHORT3_OFF;
                lastAlarmChange = currentTime;
            }
            break;
            
        case ALARM_SHORT3_OFF:
            if (currentTime - lastAlarmChange >= 100) {
                alarmState = ALARM_LONG_ON;
                lastAlarmChange = currentTime;
            }
            break;
            
        case ALARM_LONG_ON:
            if (currentTime - lastAlarmChange >= 300) {
                alarmState = ALARM_LONG_OFF;
                lastAlarmChange = currentTime;
            }
            break;
            
        case ALARM_LONG_OFF:
            if (currentTime - lastAlarmChange >= 300) {
                alarmState = ALARM_SHORT1_ON;
                lastAlarmChange = currentTime;
            }
            break;
    }
    
    // Update outputs based on alarm state
    bool buzzerState = false;
    bool ledState = false;
    
    switch (alarmState) {
        case ALARM_SHORT1_ON:
        case ALARM_SHORT2_ON:
        case ALARM_SHORT3_ON:
        case ALARM_LONG_ON:
            buzzerState = true;
            ledState = true;
            break;
            
        default:
            buzzerState = false;
            ledState = false;
    }
    
    // Apply settings
    digitalWrite(BUZZER_PIN, thresholds.buzzer_enabled && alertActive ? buzzerState : LOW);
    digitalWrite(LED_PIN, thresholds.led_enabled && alertActive ? ledState : LOW);
}

void manageRelay(bool alertCondition) {
    unsigned long currentTime = millis();
    
    if (alertCondition) {
        // Activate relay immediately if enabled
        if (thresholds.relay_enabled && !relayActive) {
            digitalWrite(RELAY_PIN, RELAY_ACTIVE_MODE);
            relayActive = true;
        }
        // Reset deactivation timer
        relayDeactivateTime = 0;
    } 
    else if (relayActive) {
        // Start deactivation timer
        if (relayDeactivateTime == 0) {
            relayDeactivateTime = currentTime;
        }
        // Deactivate after delay
        else if (currentTime - relayDeactivateTime >= (thresholds.relay_delay * 1000)) {
            digitalWrite(RELAY_PIN, !RELAY_ACTIVE_MODE);
            relayActive = false;
            relayDeactivateTime = 0;
        }
    }
}

// =============================================================================
// Settings Management
// =============================================================================
void saveSettings() {
    preferences.begin("settings", false);
    preferences.putFloat("temp_high", thresholds.temp_high);
    preferences.putFloat("temp_low", thresholds.temp_low);
    preferences.putFloat("humid_high", thresholds.humid_high);
    preferences.putFloat("humid_low", thresholds.humid_low);
    preferences.putBool("buzzer_enabled", thresholds.buzzer_enabled);
    preferences.putBool("led_enabled", thresholds.led_enabled);
    preferences.putBool("relay_enabled", thresholds.relay_enabled);
    preferences.putInt("relay_delay", thresholds.relay_delay);
    preferences.putString("username", auth.username);
    preferences.putString("password", auth.password);
    preferences.end();
    Serial.println("Settings saved");
}

void loadSettings() {
    preferences.begin("settings", true);
    thresholds.temp_high = preferences.getFloat("temp_high", 30.0);
    thresholds.temp_low = preferences.getFloat("temp_low", 15.0);
    thresholds.humid_high = preferences.getFloat("humid_high", 70.0);
    thresholds.humid_low = preferences.getFloat("humid_low", 30.0);
    thresholds.buzzer_enabled = preferences.getBool("buzzer_enabled", true);
    thresholds.led_enabled = preferences.getBool("led_enabled", true);
    thresholds.relay_enabled = preferences.getBool("relay_enabled", true);
    thresholds.relay_delay = preferences.getInt("relay_delay", 5);
    auth.username = preferences.getString("username", "admin");
    auth.password = preferences.getString("password", "password");
    preferences.end();
    Serial.println("Settings loaded");
}

// =============================================================================
// HTTP Helper Functions
// =============================================================================
String base64Decode(String input) {
    String output = "";
    const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0;
    int len = input.length();
    int val = 0, valb = -8;
    
    for (int j = 0; j < len; j++) {
        char c = input.charAt(j);
        if (c == '=') break;
        if (c >= 'A' && c <= 'Z') c = c - 'A';
        else if (c >= 'a' && c <= 'z') c = c - 'a' + 26;
        else if (c >= '0' && c <= '9') c = c - '0' + 52;
        else if (c == '+') c = 62;
        else if (c == '/') c = 63;
        else continue;
        
        val = (val << 6) + c;
        valb += 6;
        
        if (valb >= 0) {
            output += (char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return output;
}

bool checkAuth(String header) {
    int authIndex = header.indexOf("Authorization: Basic ");
    if (authIndex == -1) return false;
    
    int start = authIndex + 21;
    int end = header.indexOf("\r\n", start);
    String token = header.substring(start, end);
    String credentials = base64Decode(token);
    
    int colonIndex = credentials.indexOf(':');
    if (colonIndex == -1) return false;
    
    String username = credentials.substring(0, colonIndex);
    String password = credentials.substring(colonIndex + 1);
    
    return (username == auth.username && password == auth.password);
}

void sendUnauthorizedResponse(Client &client) {
    client.println("HTTP/1.1 401 Unauthorized");
    client.println("WWW-Authenticate: Basic realm=\"Secure Area\"");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();
    client.println("<h1>401 Unauthorized</h1>");
}

void sendChunkedResponse(Client &client, String content) {
    const int CHUNK_SIZE = 256;  // Send in 256-byte chunks
    int contentLength = content.length();
    
    for (int i = 0; i < contentLength; i += CHUNK_SIZE) {
        int endIndex = min(i + CHUNK_SIZE, contentLength);
        String chunk = content.substring(i, endIndex);
        client.print(chunk);
        delay(10);  // Small delay between chunks
    }
}

String getValue(String data, String key) {
    int startIndex = data.indexOf(key);
    if (startIndex == -1) return "";
    startIndex += key.length();
    int endIndex = data.indexOf('&', startIndex);
    if (endIndex == -1) endIndex = data.length();
    return data.substring(startIndex, endIndex);
}

// =============================================================================
// HTML Generation Functions
// =============================================================================
String generateMainPage() {
    // Get sensor data safely
    float temp, humi;
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        temp = sensorData.temperature;
        humi = sensorData.humidity;
        xSemaphoreGive(dataMutex);
    }
    
    String html = R"=====(
<!DOCTYPE html>
<html>
<head>
  <title>Environment Monitor</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="refresh" content="2">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; }
    .container { max-width: 600px; margin: 0 auto; padding: 20px; }
    .card { display: inline-block; margin: 15px; padding: 20px; border-radius: 10px; width: 40%; box-sizing: border-box; }
    .temp-card { background-color: #ffebee; border: 2px solid #f44336; }
    .humi-card { background-color: #e3f2fd; border: 2px solid #2196f3; }
    h1 { color: #333; }
    h2 { margin-top: 0; }
    .value { font-size: 32px; font-weight: bold; margin: 10px 0; }
    .alert { color: #e74c3c; font-weight: bold; margin-top: 10px; }
    .connection { margin-top: 20px; padding: 10px; border-radius: 5px; font-weight: bold; background-color: #c8e6c9; color: #2e7d32; }
    .settings-link { display: block; margin-top: 20px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Environment Monitor</h1>
    
    <div class="card temp-card">
      <h2>Temperature</h2>
      <p class="value">)=====";
    html += String(temp, 1);
    html += " °C</p>";
    
    // Temperature alerts
    if (temp > thresholds.temp_high) {
        html += "<p class=\"alert\">ALERT: Above high threshold (" + String(thresholds.temp_high, 1) + "°C)</p>";
    } else if (temp < thresholds.temp_low) {
        html += "<p class=\"alert\">ALERT: Below low threshold (" + String(thresholds.temp_low, 1) + "°C)</p>";
    }
    
    html += R"=====(
    </div>
    
    <div class="card humi-card">
      <h2>Humidity</h2>
      <p class="value">)=====";
    html += String(humi, 1);
    html += " %</p>";
    
    // Humidity alerts
    if (humi > thresholds.humid_high) {
        html += "<p class=\"alert\">ALERT: Above high threshold (" + String(thresholds.humid_high, 1) + "%)</p>";
    } else if (humi < thresholds.humid_low) {
        html += "<p class=\"alert\">ALERT: Below low threshold (" + String(thresholds.humid_low, 1) + "%)</p>";
    }
    
    html += R"=====(
    </div>
    
    <div class="connection">
      Connection: )=====";
    
    if (ethActive) html += "Ethernet";
    else if (wifiActive) html += "WiFi";
    else if (apActive) html += "Access Point";
    
    html += R"=====(
    </div>
    
    <a class="settings-link" href="/settings">Settings</a>
  </div>
</body>
</html>
)=====";
    return html;
}

String generateSettingsPage() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Settings</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;max-width:600px;margin:0 auto;padding:20px;}";
    html += "h1{text-align:center;}";
    html += ".form-group{margin-bottom:15px;}";
    html += "label{display:block;margin-bottom:5px;font-weight:bold;}";
    html += "input[type='number'],input[type='password'],input[type='checkbox']{margin-right:10px;}";
    html += "button{background-color:#4CAF50;color:white;padding:10px 20px;border:none;cursor:pointer;}";
    html += ".section{margin-bottom:25px;padding-bottom:25px;border-bottom:1px solid #eee;}";
    html += ".checkbox-group{display:flex;align-items:center;margin-bottom:10px;}";
    html += "</style></head><body>";
    html += "<h1>System Settings</h1>";
    html += "<form action='/update' method='post'>";
    
    // Temperature thresholds
    html += "<div class='section'><h2>Temperature Thresholds</h2>";
    html += "<div class='form-group'><label for='temp_high'>High Threshold (°C)</label>";
    html += "<input type='number' step='0.1' id='temp_high' name='temp_high' value='" + String(thresholds.temp_high) + "' required></div>";
    html += "<div class='form-group'><label for='temp_low'>Low Threshold (°C)</label>";
    html += "<input type='number' step='0.1' id='temp_low' name='temp_low' value='" + String(thresholds.temp_low) + "' required></div></div>";
    
    // Humidity thresholds
    html += "<div class='section'><h2>Humidity Thresholds</h2>";
    html += "<div class='form-group'><label for='humid_high'>High Threshold (%)</label>";
    html += "<input type='number' step='0.1' id='humid_high' name='humid_high' value='" + String(thresholds.humid_high) + "' required></div>";
    html += "<div class='form-group'><label for='humid_low'>Low Threshold (%)</label>";
    html += "<input type='number' step='0.1' id='humid_low' name='humid_low' value='" + String(thresholds.humid_low) + "' required></div></div>";
    
    // Alarm components settings
    html += "<div class='section'><h2>Alarm Components</h2>";
    
    // Buzzer enable
    html += "<div class='checkbox-group'>";
    html += "<input type='checkbox' id='buzzer_enabled' name='buzzer_enabled' value='1'";
    if (thresholds.buzzer_enabled) html += " checked";
    html += ">";
    html += "<label for='buzzer_enabled'>Enable Buzzer</label></div>";
    
    // LED enable
    html += "<div class='checkbox-group'>";
    html += "<input type='checkbox' id='led_enabled' name='led_enabled' value='1'";
    if (thresholds.led_enabled) html += " checked";
    html += ">";
    html += "<label for='led_enabled'>Enable LED</label></div>";
    
    // Relay enable
    html += "<div class='checkbox-group'>";
    html += "<input type='checkbox' id='relay_enabled' name='relay_enabled' value='1'";
    if (thresholds.relay_enabled) html += " checked";
    html += ">";
    html += "<label for='relay_enabled'>Enable Relay</label></div>";
    
    // Relay delay
    html += "<div class='form-group'>";
    html += "<label for='relay_delay'>Relay Turn-off Delay (seconds)</label>";
    html += "<input type='number' id='relay_delay' name='relay_delay' min='0' max='60' value='" + String(thresholds.relay_delay) + "'>";
    html += "</div></div>";
    
    // Password
    html += "<div class='section'><h2>Admin Password</h2>";
    html += "<div class='form-group'><label for='new_password'>New Password</label>";
    html += "<input type='password' id='new_password' name='new_password'></div></div>";
    
    // Submit button
    html += "<button type='submit'>Save Settings</button>";
    html += "</form>";
    html += "<p><a href='/'>Back to Dashboard</a></p>";
    html += "</body></html>";
    
    return html;
}

// =============================================================================
// HTTP Request Handling
// =============================================================================
void handleSettingsUpdate(Client &client, String request) {
    // Get content length from header
    int contentLength = 0;
    int contentStart = request.indexOf("Content-Length:");
    if (contentStart != -1) {
        int endOfLine = request.indexOf("\r\n", contentStart);
        String lengthStr = request.substring(contentStart + 15, endOfLine);
        lengthStr.trim();
        contentLength = lengthStr.toInt();
    }

    String postData = "";
    
    // Only read if content length is specified
    if (contentLength > 0) {
        Serial.print("Expecting POST data, length: ");
        Serial.println(contentLength);
        
        // Wait for data with timeout
        unsigned long startTime = millis();
        while (client.available() < contentLength && (millis() - startTime) < 2000) {
            delay(1);
        }
        
        // Read data based on content length
        for (int i = 0; i < contentLength; i++) {
            if (client.available()) {
                postData += (char)client.read();
            } else {
                break;
            }
        }
        
        Serial.print("Received POST data: ");
        Serial.println(postData);
    } else {
        Serial.println("No Content-Length header, reading available data");
        while (client.available()) {
            postData += (char)client.read();
        }
    }

    // Parse form data
    thresholds.temp_high = getValue(postData, "temp_high=").toFloat();
    thresholds.temp_low = getValue(postData, "temp_low=").toFloat();
    thresholds.humid_high = getValue(postData, "humid_high=").toFloat();
    thresholds.humid_low = getValue(postData, "humid_low=").toFloat();
    
    // Parse new alarm settings
    thresholds.buzzer_enabled = (getValue(postData, "buzzer_enabled=") == "1");
    thresholds.led_enabled = (getValue(postData, "led_enabled=") == "1");
    thresholds.relay_enabled = (getValue(postData, "relay_enabled=") == "1");
    
    // Parse relay delay
    String relayDelayStr = getValue(postData, "relay_delay=");
    if (relayDelayStr != "") {
        thresholds.relay_delay = relayDelayStr.toInt();
    }
    
    // Save new password if provided
    String newPass = getValue(postData, "new_password=");
    if (newPass != "") {
        auth.password = newPass;
    }
    
    // Save settings
    saveSettings();
    
    // Redirect to settings page
    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /settings");
    client.println();
}

void handleHTTPRequest(Client &client) {
    // Read request headers
    String request = "";
    while (client.connected() && client.available()) {
        char c = client.read();
        request += c;
        if (request.endsWith("\r\n\r\n")) break;
    }
    
    // Handle root requests
    if (request.indexOf("GET / ") != -1 || request.indexOf("GET / HTTP") != -1) {
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: text/html");
        client.println("Connection: close");
        client.println();
        sendChunkedResponse(client, generateMainPage());
    }
    // Handle settings page request
    else if (request.indexOf("GET /settings") != -1) {
        if (!checkAuth(request)) {
            sendUnauthorizedResponse(client);
        } else {
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: text/html");
            client.println("Connection: close");
            client.println();
            sendChunkedResponse(client, generateSettingsPage());
        }
    }
    // Handle settings update
    else if (request.indexOf("POST /update") != -1) {
        if (!checkAuth(request)) {
            sendUnauthorizedResponse(client);
        } else {
            handleSettingsUpdate(client, request);
        }
    }
    else {
        // Handle 404 for other paths
        client.println("HTTP/1.1 404 Not Found");
        client.println("Content-Type: text/plain");
        client.println("Connection: close");
        client.println();
        client.println("404 Not Found");
    }
    
    // Add debug information
    Serial.println("Request handled: " + request.substring(0, 50) + "...");
}

// =============================================================================
// Main Setup and Loop
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // Initialize shared data
    dataMutex = xSemaphoreCreateMutex();
    strcpy(sensorData.ipAddress, "0.0.0.0");  // Default IP
    
    // Load settings
    loadSettings();
    
    // Create tasks for both cores
    xTaskCreatePinnedToCore(
        ethernetTask,    // Task function
        "Eth_Task",      // Task name
        10000,           // Stack size
        NULL,            // Parameters
        1,               // Priority
        NULL,            // Task handle
        0                // Core 0
    );
    
    xTaskCreatePinnedToCore(
        wifiSensorTask,  // Task function
        "Wifi_Task",     // Task name
        15000,           // Stack size
        NULL,            // Parameters
        1,               // Priority
        NULL,            // Task handle
        1                // Core 1
    );
}

void loop() {
    vTaskDelete(NULL);
}