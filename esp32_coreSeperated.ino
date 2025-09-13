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
#include <ESPmDNS.h>
#include <HTTPClient.h>

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
#define BUTTON_PIN       25
#define LCD_ADDRESS      0x27
#define LCD_COLS         16
#define LCD_ROWS         2
#define BUZZER_PIN       26
#define LED_PIN          27
#define RELAY_PIN        32
#define RELAY_ACTIVE_MODE LOW
#define HOSTNAME         "temperature-vru"

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
    ALARM_OFF,
    ALARM_SHORT1_ON,
    ALARM_SHORT1_OFF,
    ALARM_SHORT2_ON,
    ALARM_SHORT2_OFF,
    ALARM_SHORT3_ON,
    ALARM_SHORT3_OFF,
    ALARM_LONG_ON,
    ALARM_LONG_OFF
};

// SMS Settings Structure
struct SMSSettings {
    String username = "Vrufan";
    String password = "Vru@fan1404";
    String sender = "500013434165493";
    String recipients = "09052084039";
    bool enabled = true;
};

struct SharedData {
    float temperature;
    float humidity;
    char ipAddress[16];
};

struct ThresholdSettings {
    float temp_high;
    float temp_low;
    float humid_high;
    float humid_low;
    bool buzzer_enabled = true;
    bool led_enabled = true;
    bool relay_enabled = true;
    int relay_delay = 5;
    SMSSettings sms;
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
HTTPClient http;

// =============================================================================
// Global Variables
// =============================================================================
SharedData sensorData;
SemaphoreHandle_t dataMutex;

ThresholdSettings thresholds = {30.0, 15.0, 70.0, 30.0};
AuthSettings auth = {"admin", "password"};

volatile bool ethActive = false;
volatile bool wifiActive = false;
volatile bool apActive = false;

volatile bool alertActive = false;
AlarmPatternState alarmState = ALARM_OFF;
unsigned long lastAlarmChange = 0;
unsigned long relayDeactivateTime = 0;
bool relayActive = false;

// SMS alert tracking
bool lastTempAlertState = false;
bool lastHumidAlertState = false;

// =============================================================================
// Web Server Optimizations
// =============================================================================

// Connection management
#define MAX_CONCURRENT_CONNECTIONS 3
#define CONNECTION_TIMEOUT_MS 5000
#define KEEP_ALIVE_TIMEOUT 30

// HTTP headers for different content types
const char HTTP_HTML_HEADER[] PROGMEM = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n";

const char HTTP_JSON_HEADER[] PROGMEM = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: no-cache, no-store, must-revalidate\r\n"
    "Connection: close\r\n\r\n";

const char HTTP_JS_HEADER[] PROGMEM = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/javascript\r\n"
    "Cache-Control: public, max-age=3600\r\n"
    "Connection: close\r\n\r\n";

const char HTTP_CSS_HEADER[] PROGMEM = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/css\r\n"
    "Cache-Control: public, max-age=3600\r\n"
    "Connection: close\r\n\r\n";

const char HTTP_REDIRECT[] PROGMEM = 
    "HTTP/1.1 303 See Other\r\n"
    "Location: /settings\r\n"
    "Connection: close\r\n\r\n";

const char HTTP_UNAUTHORIZED[] PROGMEM = 
    "HTTP/1.1 401 Unauthorized\r\n"
    "WWW-Authenticate: Basic realm=\"Secure Area\"\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n"
    "<h1>401 Unauthorized</h1>";

const char HTTP_NOT_FOUND[] PROGMEM = 
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n\r\n"
    "404 Not Found";

// Connection tracking structure
struct ConnectionState {
    Client* client;
    unsigned long lastActivity;
    bool inUse;
};

ConnectionState connections[MAX_CONCURRENT_CONNECTIONS];

// Initialize connection tracking
void initConnections() {
    for (int i = 0; i < MAX_CONCURRENT_CONNECTIONS; i++) {
        connections[i].client = nullptr;
        connections[i].inUse = false;
        connections[i].lastActivity = millis();
    }
}

// Get an available connection slot
int getFreeConnectionSlot() {
    for (int i = 0; i < MAX_CONCURRENT_CONNECTIONS; i++) {
        if (!connections[i].inUse) {
            connections[i].inUse = true;
            connections[i].lastActivity = millis();
            return i;
        }
    }
    return -1;
}

// Clean up stale connections
void cleanupConnections() {
    unsigned long currentTime = millis();
    for (int i = 0; i < MAX_CONCURRENT_CONNECTIONS; i++) {
        if (connections[i].inUse && 
            (currentTime - connections[i].lastActivity > CONNECTION_TIMEOUT_MS)) {
            if (connections[i].client && connections[i].client->connected()) {
                connections[i].client->stop();
            }
            connections[i].inUse = false;
            connections[i].client = nullptr;
        }
    }
}

// =============================================================================
// LCD Manager Class
// =============================================================================
class LCDManager {
private:
    DisplayMode lastMode = SENSOR_MODE;
    float lastTemp = -100.0;
    float lastHumi = -100.0;

public:
    void init() {
        lcd.init();
        lcd.backlight();
        lcd.clear();
    }

    void update(DisplayMode mode, float temp, float humi, const char* ip) {
                
        // Handle mode changes
        if (mode != lastMode) {
            lcd.clear();
            lastMode = mode;
            
            if (mode == SENSOR_MODE) {
                lcd.setCursor(0, 0);
                lcd.print("Temp: ");
                lcd.setCursor(6, 0);
                lcd.print(temp, 1);
                lcd.print("C ");

                lcd.setCursor(0, 1);
                lcd.print("Hum:  ");
                lcd.setCursor(6, 1);
                lcd.print(humi, 1);
                lcd.print("% ");
            }
            else {
              updateIPMode(ip);
            }
        }

        if (mode == SENSOR_MODE) {
            updateSensorMode(temp, humi);
        } 
        else if (mode == IP_MODE) {
            updateIPMode(ip);
        }
    }

private:
    void updateSensorMode(float temp, float humi) {
        // Update temperature if changed
        if (abs(temp - lastTemp) > 0.09) {
            lcd.setCursor(0, 0);
            lcd.print("Temp: ");
            lcd.setCursor(6, 0);
            lcd.print(temp, 1);
            lcd.print("C ");
            lastTemp = temp;
        }

        // Update humidity if changed
        if (abs(humi - lastHumi) > 0.09) {
            lcd.setCursor(0, 1);
            lcd.print("Hum:  ");
            lcd.setCursor(6, 1);
            lcd.print(humi, 1);
            lcd.print("% ");
            lastHumi = humi;
        }

    }

    void updateIPMode(const char* ip) {
            lcd.setCursor(0, 0);
            lcd.print("IP Address:");
            lcd.setCursor(0, 1);
            lcd.print(ip);
    }
};

LCDManager lcdManager;
bool lcdAvailable = false;

// =============================================================================
// Function Declarations
// =============================================================================
void updateIPAddress(IPAddress ip);
void checkAndSendSMSAlerts(bool tempAlert, bool humiAlert);
void updateAlarmPattern();
void manageRelay(bool alertCondition);
bool checkAuth(String header);
void handleSettingsUpdate(Client &client, String request);
void loadSettings();
String base64Decode(String input);
String getValue(String data, String key);
void sendSensorDataJSON(Client &client);
void sendMainPage(Client &client);
void sendSettingsPage(Client &client);
void sendJavaScript(Client &client);
void sendCSS(Client &client);
String readRequestWithTimeout(Client &client, unsigned long timeoutMs);
void handleHTTPRequest(Client &client);

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
        
        // Start mDNS service
        if (!MDNS.begin(HOSTNAME)) {
            Serial.println("Error setting up mDNS responder!");
        } else {
            Serial.println("mDNS responder started");
            MDNS.addService("http", "tcp", 80);
        }
        
        // Disable WiFi
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.println("[C0] WIFI disconnected!");

        ethActive = true;
        ethServer.begin();
        
        // Initialize connection tracking
        initConnections();
        
        // Main Ethernet processing loop
        while (true) {
            cleanupConnections();
            
            EthernetClient client = ethServer.available();
            if (client) {
                int slot = getFreeConnectionSlot();
                if (slot >= 0) {
                    connections[slot].client = &client;
                    connections[slot].lastActivity = millis();
                    
                    // Handle request with timeout protection
                    handleHTTPRequest(client);
                    
                    connections[slot].inUse = false;
                    connections[slot].client = nullptr;
                } else {
                    // Reject connection if no slots available
                    client.println("HTTP/1.1 503 Service Unavailable");
                    client.println("Retry-After: 5");
                    client.println("Connection: close");
                    client.println();
                    client.println("Server busy. Please try again later.");
                    client.stop();
                }
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
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Initialize alarm components
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    digitalWrite(RELAY_PIN, !RELAY_ACTIVE_MODE);

    // Initialize LCD
    Wire.begin();
    Wire.beginTransmission(LCD_ADDRESS);
    if (Wire.endTransmission() == 0) {
        lcdManager.init();
        lcd.setCursor(0, 0);
        lcd.print("System Starting");
        lcdAvailable = true;
    } else {
        Serial.println("LCD not found. Continuing without LCD.");
        lcdAvailable = false;
    }
    
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
            
            // Start mDNS service for WiFi
            if (!MDNS.begin(HOSTNAME)) {
                Serial.println("Error setting up mDNS responder!");
            } else {
                Serial.println("mDNS responder started");
                MDNS.addService("http", "tcp", 80);
            }
        } else {
            Serial.println("\n[C1] WiFi failed! Starting AP...");
            WiFi.softAP(AP_SSID, AP_PASS);
            Serial.print("[C1] AP IP: ");
            Serial.println(WiFi.softAPIP());
            updateIPAddress(WiFi.softAPIP());
            apActive = true;
            wifiServer.begin();
            
            // Start mDNS service for AP mode
            if (!MDNS.begin(HOSTNAME)) {
                Serial.println("Error setting up mDNS responder!");
            } else {
                Serial.println("mDNS responder started");
                MDNS.addService("http", "tcp", 80);
            }
        }
        
        // Initialize connection tracking for WiFi/AP mode
        initConnections();
    }
    
    // Main sensor and LCD loop
    unsigned long lastSensorTime = 0;
    unsigned long lastLCDUpdate = 0;
    DisplayMode displayMode = SENSOR_MODE;
    lcd.clear();
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
        if (digitalRead(BUTTON_PIN) == LOW) {
            if (!buttonActive) {
                buttonActive = true;
                buttonPressStart = currentTime;
            }
            
            if (buttonActive && (currentTime - buttonPressStart >= 500)) {
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
                alarmState = ALARM_OFF;
            }
        }
        
        // Check and send SMS alerts
        checkAndSendSMSAlerts(tempAlert, humiAlert);
        
        // Update alarm pattern
        if (alertActive) {
            updateAlarmPattern();
        } else {
            digitalWrite(BUZZER_PIN, LOW);
            digitalWrite(LED_PIN, LOW);
        }
        
        // Manage relay state
        manageRelay(alertCondition);
        
        // Update LCD every 500ms
        if (currentTime - lastLCDUpdate >= 500) {
            float temp, humi;
            char ip[16];
            
            if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
                temp = sensorData.temperature;
                humi = sensorData.humidity;
                strcpy(ip, sensorData.ipAddress);
                
                xSemaphoreGive(dataMutex);
            }
            
            if (lcdAvailable) {
                lcdManager.update(displayMode, temp, humi, ip);
            }
            lastLCDUpdate = currentTime;
        }
        
        // Handle WiFi/AP clients with optimized connection management
        if (!ethActive) {
            cleanupConnections();
            
            WiFiClient client = wifiServer.available();
            if (client) {
                int slot = getFreeConnectionSlot();
                if (slot >= 0) {
                    connections[slot].client = &client;
                    connections[slot].lastActivity = millis();
                    
                    // Handle request with timeout protection
                    handleHTTPRequest(client);
                    
                    connections[slot].inUse = false;
                    connections[slot].client = nullptr;
                } else {
                    // Reject connection if no slots available
                    client.println("HTTP/1.1 503 Service Unavailable");
                    client.println("Retry-After: 5");
                    client.println("Connection: close");
                    client.println();
                    client.println("Server busy. Please try again later.");
                    client.stop();
                }
            }
        }

        vTaskDelay(5); // Reduced delay for better responsiveness
    }
}

// =============================================================================
// HTTP Handling Functions
// =============================================================================

// Send sensor data as JSON
void sendSensorDataJSON(Client &client) {
    float temp, humi;
    
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        temp = sensorData.temperature;
        humi = sensorData.humidity;
        xSemaphoreGive(dataMutex);
    }
    
    client.print(FPSTR(HTTP_JSON_HEADER));
    client.print("{\"temperature\":");
    client.print(temp, 1);
    client.print(",\"humidity\":");
    client.print(humi, 1);
    client.print(",\"temp_high\":");
    client.print(thresholds.temp_high, 1);
    client.print(",\"temp_low\":");
    client.print(thresholds.temp_low, 1);
    client.print(",\"humid_high\":");
    client.print(thresholds.humid_high, 1);
    client.print(",\"humid_low\":");
    client.print(thresholds.humid_low, 1);
    client.print(",\"alert\":");
    client.print(alertActive ? "true" : "false");
    client.print("}");
}

// Send main page HTML
void sendMainPage(Client &client) {
    client.print(FPSTR(HTTP_HTML_HEADER));
    
    client.print(R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Environment Monitor</title>
  <link rel="stylesheet" href="/style.css">
  <script src="/script.js"></script>
</head>
<body>
  <div class="container">
    <header>
      <h1>Environment Monitor</h1>
    </header>
    
    <main class="sensor-grid">
      <div class="sensor-card">
        <div class="sensor-icon">🌡️</div>
        <div class="sensor-value" id="temperature">--.- °C</div>
        <div class="sensor-label">Temperature</div>
      </div>
      
      <div class="sensor-card">
        <div class="sensor-icon">💧</div>
        <div class="sensor-value" id="humidity">--.- %</div>
        <div class="sensor-label">Humidity</div>
      </div>
    </main>
    
    <div class="alert-container" id="alert-container">
      <!-- Alerts will be dynamically inserted here -->
    </div>
    
    <footer>
      <a href="/settings" class="settings-btn">Settings</a>
    </footer>
  </div>
</body>
</html>
)=====");
}

// Send CSS with dark theme and responsive design
void sendCSS(Client &client) {
    client.print(FPSTR(HTTP_CSS_HEADER));
    
    client.print(R"=====(
:root {
  --primary-dark: #0d1b2a;
  --secondary-dark: #1b263b;
  --accent-blue: #2196f3;
  --text-primary: #e0e1dd;
  --text-secondary: #b0b0b0;
  --alert-color: #ff6b6b;
  --card-bg: #1b263b;
}

* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

body {
  font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
  background-color: var(--primary-dark);
  color: var(--text-primary);
  line-height: 1.6;
  min-height: 100vh;
}

.container {
  max-width: 1200px;
  margin: 0 auto;
  padding: 20px;
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}

header {
  text-align: center;
  margin-bottom: 2rem;
}

h1 {
  font-size: 2.5rem;
  font-weight: 300;
  color: var(--accent-blue);
  margin-bottom: 0.5rem;
}

.sensor-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
  gap: 2rem;
  margin-bottom: 2rem;
  flex: 1;
}

.sensor-card {
  background: var(--card-bg);
  border-radius: 16px;
  padding: 2rem;
  text-align: center;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2);
  border: 1px solid rgba(255, 255, 255, 0.1);
  transition: transform 0.2s ease;
}

.sensor-card:hover {
  transform: translateY(-4px);
}

.sensor-icon {
  font-size: 3rem;
  margin-bottom: 1rem;
}

.sensor-value {
  font-size: 3.5rem;
  font-weight: 700;
  color: var(--accent-blue);
  margin-bottom: 0.5rem;
  text-shadow: 0 4px 8px rgba(33, 150, 243, 0.3);
}

.sensor-label {
  font-size: 1.2rem;
  color: var(--text-secondary);
  text-transform: uppercase;
  letter-spacing: 1px;
}

.alert-container {
  margin-bottom: 2rem;
}

.alert {
  background: var(--card-bg);
  border-left: 4px solid var(--alert-color);
  padding: 1rem 1.5rem;
  border-radius: 8px;
  margin-bottom: 1rem;
  animation: slideIn 0.3s ease;
}

.alert-content {
  display: flex;
  align-items: center;
  gap: 0.75rem;
}

.alert-icon {
  font-size: 1.5rem;
  color: var(--alert-color);
}

.alert-message {
  font-size: 1.1rem;
  font-weight: 500;
}

footer {
  text-align: center;
  margin-top: auto;
}

.settings-btn {
  display: inline-block;
  background: var(--accent-blue);
  color: white;
  text-decoration: none;
  padding: 1rem 2rem;
  border-radius: 8px;
  font-weight: 600;
  transition: background-color 0.2s ease;
  border: none;
  cursor: pointer;
  font-size: 1.1rem;
}

.settings-btn:hover {
  background: #1976d2;
  transform: translateY(-2px);
}

@keyframes slideIn {
  from {
    opacity: 0;
    transform: translateX(-20px);
  }
  to {
    opacity: 1;
    transform: translateX(0);
  }
}

/* Responsive Design */
@media (max-width: 768px) {
  .container {
    padding: 1rem;
  }
  
  h1 {
    font-size: 2rem;
  }
  
  .sensor-grid {
    grid-template-columns: 1fr;
    gap: 1.5rem;
  }
  
  .sensor-card {
    padding: 1.5rem;
  }
  
  .sensor-value {
    font-size: 3rem;
  }
  
  .sensor-icon {
    font-size: 2.5rem;
  }
}

@media (max-width: 480px) {
  h1 {
    font-size: 1.75rem;
  }
  
  .sensor-value {
    font-size: 2.5rem;
  }
  
  .settings-btn {
    width: 100%;
    text-align: center;
  }
}
)=====");
}

// Send JavaScript with updated alert handling
void sendJavaScript(Client &client) {
    client.print(FPSTR(HTTP_JS_HEADER));
    
    client.print(R"=====(
let lastUpdateTime = 0;
const UPDATE_INTERVAL = 2000;

function updateSensorData() {
  fetch('/data')
    .then(response => response.json())
    .then(data => {
      document.getElementById('temperature').textContent = data.temperature.toFixed(1) + ' °C';
      document.getElementById('humidity').textContent = data.humidity.toFixed(1) + ' %';
      updateAlerts(data);
      lastUpdateTime = Date.now();
    })
    .catch(error => {
      console.error('Error fetching sensor data:', error);
    });
}

function updateAlerts(data) {
  const alertContainer = document.getElementById('alert-container');
  alertContainer.innerHTML = '';
  
  // Temperature alerts
  if (data.temperature > data.temp_high) {
    addAlert('🌡️', `Temperature Alert: ${data.temperature.toFixed(1)}°C (Above ${data.temp_high.toFixed(1)}°C)`);
  } else if (data.temperature < data.temp_low) {
    addAlert('🌡️', `Temperature Alert: ${data.temperature.toFixed(1)}°C (Below ${data.temp_low.toFixed(1)}°C)`);
  }
  
  // Humidity alerts
  if (data.humidity > data.humid_high) {
    addAlert('💧', `Humidity Alert: ${data.humidity.toFixed(1)}% (Above ${data.humid_high.toFixed(1)}%)`);
  } else if (data.humidity < data.humid_low) {
    addAlert('💧', `Humidity Alert: ${data.humidity.toFixed(1)}% (Below ${data.humid_low.toFixed(1)}%)`);
  }
}

function addAlert(icon, message) {
  const alertContainer = document.getElementById('alert-container');
  const alertDiv = document.createElement('div');
  alertDiv.className = 'alert';
  alertDiv.innerHTML = `
    <div class="alert-content">
      <span class="alert-icon">${icon}</span>
      <span class="alert-message">${message}</span>
    </div>
  `;
  alertContainer.appendChild(alertDiv);
}

// Start updates when page loads
window.addEventListener('load', function() {
  updateSensorData(); // First update
  setInterval(updateSensorData, UPDATE_INTERVAL); // Periodic updates
});
)=====");
}
void sendSettingsPage(Client &client) {
    client.print(FPSTR(HTTP_HTML_HEADER));
    
    client.print(R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>System Settings - Environment Monitor</title>
  <link rel="stylesheet" href="/style.css">
  <style>
    .settings-container {
      max-width: 800px;
      margin: 0 auto;
    }
    
    .settings-header {
      text-align: center;
      margin-bottom: 2rem;
    }
    
    .settings-form {
      background: var(--card-bg);
      border-radius: 16px;
      padding: 2rem;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2);
      border: 1px solid rgba(255, 255, 255, 0.1);
    }
    
    .form-section {
      margin-bottom: 2.5rem;
      padding-bottom: 2rem;
      border-bottom: 1px solid rgba(255, 255, 255, 0.1);
    }
    
    .form-section:last-of-type {
      border-bottom: none;
      margin-bottom: 0;
      padding-bottom: 0;
    }
    
    .form-section h2 {
      color: var(--accent-blue);
      margin-bottom: 1.5rem;
      font-size: 1.5rem;
      font-weight: 600;
    }
    
    .form-group {
      margin-bottom: 1.5rem;
    }
    
    .form-group label {
      display: block;
      margin-bottom: 0.5rem;
      font-weight: 500;
      color: var(--text-primary);
    }
    
    .form-group input[type="number"],
    .form-group input[type="text"],
    .form-group input[type="password"] {
      width: 100%;
      padding: 0.75rem 1rem;
      border-radius: 8px;
      border: 1px solid rgba(255, 255, 255, 0.2);
      background: rgba(0, 0, 0, 0.2);
      color: var(--text-primary);
      font-size: 1rem;
      transition: border-color 0.2s ease;
    }
    
    .form-group input:focus {
      outline: none;
      border-color: var(--accent-blue);
    }
    
    .checkbox-group {
      display: flex;
      align-items: center;
      margin-bottom: 1rem;
    }
    
    .checkbox-group input[type="checkbox"] {
      margin-right: 0.75rem;
      width: 18px;
      height: 18px;
      accent-color: var(--accent-blue);
    }
    
    .checkbox-group label {
      margin-bottom: 0;
      font-weight: normal;
    }
    
    .form-actions {
      text-align: center;
      margin-top: 2rem;
    }
    
    .submit-btn {
      background: var(--accent-blue);
      color: white;
      border: none;
      padding: 1rem 2.5rem;
      border-radius: 8px;
      font-size: 1.1rem;
      font-weight: 600;
      cursor: pointer;
      transition: background-color 0.2s ease;
    }
    
    .submit-btn:hover {
      background: #1976d2;
    }
    
    .back-link {
      display: inline-block;
      margin-top: 1.5rem;
      color: var(--accent-blue);
      text-decoration: none;
      font-weight: 500;
    }
    
    .back-link:hover {
      text-decoration: underline;
    }
    
    @media (max-width: 768px) {
      .settings-form {
        padding: 1.5rem;
      }
      
      .form-section {
        margin-bottom: 2rem;
        padding-bottom: 1.5rem;
      }
    }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <h1>System Settings</h1>
    </header>
    
    <main class="settings-container">
      <form action="/update" method="post" class="settings-form">
        <!-- Temperature Thresholds -->
        <div class="form-section">
          <h2>Temperature Thresholds</h2>
          <div class="form-group">
            <label for="temp_high">High Threshold (°C)</label>
            <input type="number" step="0.1" id="temp_high" name="temp_high" value=")=====");
    client.print(thresholds.temp_high);
    client.print(R"=====(" required>
          </div>
          <div class="form-group">
            <label for="temp_low">Low Threshold (°C)</label>
            <input type="number" step="0.1" id="temp_low" name="temp_low" value=")=====");
    client.print(thresholds.temp_low);
    client.print(R"=====(" required>
          </div>
        </div>
        
        <!-- Humidity Thresholds -->
        <div class="form-section">
          <h2>Humidity Thresholds</h2>
          <div class="form-group">
            <label for="humid_high">High Threshold (%)</label>
            <input type="number" step="0.1" id="humid_high" name="humid_high" value=")=====");
    client.print(thresholds.humid_high);
    client.print(R"=====(" required>
          </div>
          <div class="form-group">
            <label for="humid_low">Low Threshold (%)</label>
            <input type="number" step="0.1" id="humid_low" name="humid_low" value=")=====");
    client.print(thresholds.humid_low);
    client.print(R"=====(" required>
          </div>
        </div>
        
        <!-- Alarm Components -->
        <div class="form-section">
          <h2>Alarm Components</h2>
          <div class="checkbox-group">
            <input type="checkbox" id="buzzer_enabled" name="buzzer_enabled" value="1")=====");
    if (thresholds.buzzer_enabled) client.print(" checked");
    client.print(R"=====(>
            <label for="buzzer_enabled">Enable Buzzer</label>
          </div>
          <div class="checkbox-group">
            <input type="checkbox" id="led_enabled" name="led_enabled" value="1")=====");
    if (thresholds.led_enabled) client.print(" checked");
    client.print(R"=====(>
            <label for="led_enabled">Enable LED</label>
          </div>
          <div class="checkbox-group">
            <input type="checkbox" id="relay_enabled" name="relay_enabled" value="1")=====");
    if (thresholds.relay_enabled) client.print(" checked");
    client.print(R"=====(>
            <label for="relay_enabled">Enable Relay</label>
          </div>
          <div class="form-group">
            <label for="relay_delay">Relay Turn-off Delay (seconds)</label>
            <input type="number" id="relay_delay" name="relay_delay" min="0" max="60" value=")=====");
    client.print(thresholds.relay_delay);
    client.print(R"=====(">
          </div>
        </div>
        
        <!-- SMS Settings -->
        <div class="form-section">
          <h2>SMS Settings</h2>
          <div class="checkbox-group">
            <input type="checkbox" id="sms_enabled" name="sms_enabled" value="1")=====");
    if (thresholds.sms.enabled) client.print(" checked");
    client.print(R"=====(>
            <label for="sms_enabled">Enable SMS Alerts</label>
          </div>
          <div class="form-group">
            <label for="sms_username">SMS Username</label>
            <input type="text" id="sms_username" name="sms_username" value=")=====");
    client.print(thresholds.sms.username);
    client.print(R"=====(">
          </div>
          <div class="form-group">
            <label for="sms_password">SMS Password</label>
            <input type="password" id="sms_password" name="sms_password" value=")=====");
    client.print(thresholds.sms.password);
    client.print(R"=====(">
          </div>
          <div class="form-group">
            <label for="sms_sender">Sender Number</label>
            <input type="text" id="sms_sender" name="sms_sender" value=")=====");
    client.print(thresholds.sms.sender);
    client.print(R"=====(">
          </div>
          <div class="form-group">
            <label for="sms_recipients">Recipient Numbers (comma separated)</label>
            <input type="text" id="sms_recipients" name="sms_recipients" value=")=====");
    client.print(thresholds.sms.recipients);
    client.print(R"=====(">
          </div>
        </div>
        
        <!-- Admin Password -->
        <div class="form-section">
          <h2>Admin Password</h2>
          <div class="form-group">
            <label for="new_password">New Password</label>
            <input type="password" id="new_password" name="new_password">
          </div>
        </div>
        
        <!-- Form Actions -->
        <div class="form-actions">
          <button type="submit" class="submit-btn">Save Settings</button>
          <a href="/" class="back-link">Back to Dashboard</a>
        </div>
      </form>
    </main>
  </div>
</body>
</html>
)=====");
}

// Read request with timeout
String readRequestWithTimeout(Client &client, unsigned long timeoutMs) {
    String request = "";
    unsigned long startTime = millis();
    
    while (client.connected() && (millis() - startTime < timeoutMs)) {
        if (client.available()) {
            char c = client.read();
            request += c;
            if (request.endsWith("\r\n\r\n")) break;
        }
        vTaskDelay(1);
    }
    
    return request;
}


void handleHTTPRequest(Client &client) {
    // Read request with timeout
    String request = readRequestWithTimeout(client, 2000);
    
    if (request.length() == 0) {
        client.stop();
        return;
    }
    
    // Handle data request
    if (request.indexOf("GET /data") != -1) {
        sendSensorDataJSON(client);
    }
    // Handle CSS request
    else if (request.indexOf("GET /style.css") != -1) {
        sendCSS(client);
    }
    // Handle JavaScript request
    else if (request.indexOf("GET /script.js") != -1) {
        sendJavaScript(client);
    }
    // Handle root requests
    else if (request.indexOf("GET / ") != -1 || request.indexOf("GET / HTTP") != -1) {
        sendMainPage(client);
    }
    // Handle settings page request
    else if (request.indexOf("GET /settings") != -1) {
        if (!checkAuth(request)) {
            client.print(FPSTR(HTTP_UNAUTHORIZED));
        } else {
            sendSettingsPage(client);
        }
    }
    // Handle settings update
    else if (request.indexOf("POST /update") != -1) {
        if (!checkAuth(request)) {
            client.print(FPSTR(HTTP_UNAUTHORIZED));
        } else {
            handleSettingsUpdate(client, request);
            client.print(FPSTR(HTTP_REDIRECT));
        }
    }
    else {
        client.print(FPSTR(HTTP_NOT_FOUND));
    }
    
    client.stop();
}

// =============================================================================
// SMS Functions
// =============================================================================
// Check if SMS is configured properly
bool isSMSConfigured() {
    return thresholds.sms.username != "" && 
           thresholds.sms.password != "" && 
           thresholds.sms.sender != "" && 
           thresholds.sms.recipients != "";
}

// Send SMS alert
void sendSMSAlert(const String& message) {
    if (!thresholds.sms.enabled || !isSMSConfigured()) {
        Serial.println("SMS not configured or disabled");
        return;
    }

    String soapRequest = R"(
    <soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" 
                     xmlns:tem="http://tempuri.org/">
      <soapenv:Header/>
      <soapenv:Body>
        <tem:SendSimpleSMS>
          <tem:username>)" + thresholds.sms.username + R"(</tem:username>
          <tem:password>)" + thresholds.sms.password + R"(</tem:password>
          <tem:to>)" + thresholds.sms.recipients + R"(</tem:to>
          <tem:from>)" + thresholds.sms.sender + R"(</tem:from>
          <tem:text>)" + message + R"(</tem:text>
          <tem:isflash>false</tem:isflash>
        </tem:SendSimpleSMS>
      </soapenv:Body>
    </soapenv:Envelope>
    )";

    http.begin("http://linepayamak.ir/Post/Send.asmx");
    http.addHeader("Content-Type", "text/xml; charset=utf-8");
    http.addHeader("SOAPAction", "http://tempuri.org/SendSimpleSMS");
    
    int httpCode = http.POST(soapRequest);
    if (httpCode == HTTP_CODE_OK) {
        Serial.println("SMS alert sent: " + message);
    } else {
        Serial.println("SMS send failed: " + http.errorToString(httpCode));
    }
    http.end();
}

// Check and send SMS alerts when alert state changes
void checkAndSendSMSAlerts(bool tempAlert, bool humiAlert) {
    float temp, humi;
    
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        temp = sensorData.temperature;
        humi = sensorData.humidity;
        xSemaphoreGive(dataMutex);
    }
    
    // Temperature alert
    if (tempAlert && !lastTempAlertState) {
        String message = "Temperature Alert: " + String(temp, 1) + "°C";
        sendSMSAlert(message);
    }
    
    // Humidity alert
    if (humiAlert && !lastHumidAlertState) {
        String message = "Humidity Alert: " + String(humi, 1) + "%";
        sendSMSAlert(message);
    }
    
    // Update previous states
    lastTempAlertState = tempAlert;
    lastHumidAlertState = humiAlert;
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
    
    // Save SMS settings
    preferences.putBool("sms_enabled", thresholds.sms.enabled);
    preferences.putString("sms_username", thresholds.sms.username);
    preferences.putString("sms_password", thresholds.sms.password);
    preferences.putString("sms_sender", thresholds.sms.sender);
    preferences.putString("sms_recipients", thresholds.sms.recipients);
    
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
    
    // Load SMS settings
    thresholds.sms.enabled = preferences.getBool("sms_enabled", true);
    thresholds.sms.username = preferences.getString("sms_username", "Vrufan");
    thresholds.sms.password = preferences.getString("sms_password", "Vru@fan1404");
    thresholds.sms.sender = preferences.getString("sms_sender", "500013434165493");
    thresholds.sms.recipients = preferences.getString("sms_recipients", "09052084039");
    
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
    const int CHUNK_SIZE = 256;
    int contentLength = content.length();
    
    for (int i = 0; i < contentLength; i += CHUNK_SIZE) {
        int endIndex = min(i + CHUNK_SIZE, contentLength);
        String chunk = content.substring(i, endIndex);
        client.print(chunk);
        delay(10);
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
    
    // Parse alarm components settings
    thresholds.buzzer_enabled = (getValue(postData, "buzzer_enabled=") == "1");
    thresholds.led_enabled = (getValue(postData, "led_enabled=") == "1");
    thresholds.relay_enabled = (getValue(postData, "relay_enabled=") == "1");
    
    // Parse relay delay
    String relayDelayStr = getValue(postData, "relay_delay=");
    if (relayDelayStr != "") {
        thresholds.relay_delay = relayDelayStr.toInt();
    }
    
    // Parse SMS settings
    thresholds.sms.enabled = (getValue(postData, "sms_enabled=") == "1");
    thresholds.sms.username = getValue(postData, "sms_username=");
    thresholds.sms.password = getValue(postData, "sms_password=");
    thresholds.sms.sender = getValue(postData, "sms_sender=");
    thresholds.sms.recipients = getValue(postData, "sms_recipients=");
    
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

// =============================================================================
// Main Setup and Loop 
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // Initialize shared data
    dataMutex = xSemaphoreCreateMutex();
    strcpy(sensorData.ipAddress, "0.0.0.0");
    
    // Load settings
    loadSettings();
    
    // Create tasks for both cores
    xTaskCreatePinnedToCore(
        ethernetTask,
        "Eth_Task",
        10000,
        NULL,
        1,
        NULL,
        0
    );
    
    xTaskCreatePinnedToCore(
        wifiSensorTask,
        "Wifi_Task",
        15000,
        NULL,
        1,
        NULL,
        1
    );
}

void loop() {
    vTaskDelete(NULL);
}
