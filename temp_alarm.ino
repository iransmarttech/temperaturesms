#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <HTTPClient.h>

// ====================
// Hardware Configuration
// ====================
#define DHTPIN 4          // GPIO pin for DHT22
#define DHTTYPE DHT22     // DHT sensor type
#define BUTTON_PIN 5      // GPIO pin for IP display button
#define LCD_ADDRESS 0x27  // I2C address for LCD (common: 0x27 or 0x3F)
#define LCD_COLS 16       // LCD columns
#define LCD_ROWS 2        // LCD rows

// ====================
// Device Constants
// ====================
const char* DEVICE_NAME = "SmartMonitor";
const char* SOFTWARE_VERSION = "1.0";

// ====================
// Global Objects
// ====================
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
WebServer server(80);
Preferences preferences;
HTTPClient http;

// ====================
// Sensor Data Structure
// ====================
struct SensorData {
  float temperature;
  float humidity;
  bool tempAlertActive;
  bool humidAlertActive;
} sensor;

// ====================
// Device Configuration
// ====================
struct DeviceConfig {
  // Network settings
  String wifiSSID;
  String wifiPassword;
  
  // Alert thresholds
  float tempHighThreshold;
  float tempLowThreshold;
  float humidHighThreshold;
  float humidLowThreshold;
  
  // SMS settings
  String smsUsername;
  String smsPassword;
  String smsSender;
  String smsRecipients;  // Comma-separated numbers
  
  // Admin credentials
  String adminUser;
  String adminPass;
};

DeviceConfig config;

// ====================
// System State
// ====================
enum DisplayMode { SENSOR_DATA, IP_ADDRESS };
DisplayMode displayMode = SENSOR_DATA;
unsigned long lastDisplaySwitch = 0;
const long DISPLAY_DURATION = 5000;  // 5 seconds

// ====================
// Setup Functions
// ====================
void setup() {
  // Initialize serial communication
  Serial.begin(115200);
  
  // Initialize hardware
  initializeHardware();
  
  // Load configuration from storage
  loadConfiguration();
  
  // Connect to network
  connectToNetwork();
  
  // Initialize web server
  setupWebServer();
  
  // Initial sensor reading
  readSensorData();
  
  Serial.println("System initialization complete");
}

void initializeHardware() {
  // Initialize DHT sensor
  dht.begin();
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");
  
  // Initialize button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void loadConfiguration() {
  preferences.begin("config", false);
  
  // Load WiFi credentials
  config.wifiSSID = preferences.getString("wifiSSID", "Museum-3(2G)");
  config.wifiPassword = preferences.getString("wifiPass", "");
  
  // Load thresholds (default values)
  config.tempHighThreshold = preferences.getFloat("tempHigh", 30.0);
  config.tempLowThreshold = preferences.getFloat("tempLow", 15.0);
  config.humidHighThreshold = preferences.getFloat("humidHigh", 70.0);
  config.humidLowThreshold = preferences.getFloat("humidLow", 30.0);
  
  // Load SMS settings
  config.smsUsername = preferences.getString("smsUser", "Vrufan");
  config.smsPassword = preferences.getString("smsPass", "Vru@fan1404");
  config.smsSender = preferences.getString("smsSender", "500013434165493");
  config.smsRecipients = preferences.getString("smsRecipients", "09052084039");
  
  // Load admin credentials (default: admin/admin)
  config.adminUser = preferences.getString("adminUser", "morteza");
  config.adminPass = preferences.getString("adminPass", "1234");
  
  preferences.end();
  
  // Initialize alert states
  sensor.tempAlertActive = false;
  sensor.humidAlertActive = false;
}

// ====================
// Network Functions
// ====================
void connectToNetwork() {
  if (config.wifiSSID == "") {
    Serial.println("WiFi credentials not set!");
    lcd.clear();
    lcd.print("WiFi not set!");
    return;
  }

  Serial.print("Connecting to WiFi: ");
  Serial.println(config.wifiSSID);
  
  lcd.clear();
  lcd.print("Connecting WiFi");
  lcd.setCursor(0, 1);
  lcd.print(config.wifiSSID.substring(0, 16));
  
  WiFi.begin(config.wifiSSID.c_str(), config.wifiPassword.c_str());
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  updateLCD();
}

// ====================
// Web Server Functions
// ====================
void setupWebServer() {
  // Authentication middleware
  server.on("/", HTTP_GET, []() {
    if (!authenticate()) return server.requestAuthentication();
    handleDashboard();
  });
  
  server.on("/settings", HTTP_GET, []() {
    if (!authenticate()) return server.requestAuthentication();
    handleSettingsPage();
  });
  
  server.on("/update", HTTP_POST, []() {
    if (!authenticate()) return server.requestAuthentication();
    handleSettingsUpdate();
  });
  
  server.on("/sensor-data", HTTP_GET, handleSensorDataAPI);
  
  server.begin();
  Serial.println("HTTP server started");
}

bool authenticate() {
  return server.authenticate(config.adminUser.c_str(), config.adminPass.c_str());
}

// ====================
// Web Handlers
// ====================
void handleDashboard() {
  String html = R"(
  <!DOCTYPE html>
  <html>
  <head>
    <title>Environment Monitor</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial, sans-serif; margin: 20px; }
      .card { background: #f9f9f9; border-radius: 8px; padding: 20px; margin: 10px 0; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
      .sensor-value { font-size: 24px; font-weight: bold; color: #2c3e50; }
      .alert { color: #e74c3c; }
      .normal { color: #27ae60; }
      form { display: grid; gap: 10px; max-width: 400px; }
      input, button { padding: 10px; }
      button { background: #3498db; color: white; border: none; cursor: pointer; }
    </style>
    <script>
      function updateSensorData() {
        fetch('/sensor-data')
          .then(response => response.json())
          .then(data => {
            document.getElementById('temp-value').innerText = data.temperature.toFixed(1) + '°C';
            document.getElementById('humid-value').innerText = data.humidity.toFixed(1) + '%';
            
            document.getElementById('temp-value').className = 
              (data.temperature > data.tempHigh || data.temperature < data.tempLow) ? 
              'sensor-value alert' : 'sensor-value normal';
              
            document.getElementById('humid-value').className = 
              (data.humidity > data.humidHigh || data.humidity < data.humidLow) ? 
              'sensor-value alert' : 'sensor-value normal';
          });
      }
      
      setInterval(updateSensorData, 3000);
      window.onload = updateSensorData;
    </script>
  </head>
  <body>
    <h1>Environment Monitor</h1>
    
    <div class="card">
      <h2>Current Status</h2>
      <p>Temperature: <span id="temp-value" class="sensor-value">--.-°C</span></p>
      <p>Humidity: <span id="humid-value" class="sensor-value">--.-%</span></p>
      <p>Device IP: )" + WiFi.localIP().toString() + R"(</p>
      <p>Uptime: <span id="uptime">)" + String(millis() / 1000) + R"(s</span></p>
    </div>
    
    <p><a href="/settings">Settings & Alerts Configuration</a></p>
  </body>
  </html>
  )";
  
  server.send(200, "text/html", html);
}

void handleSettingsPage() {
  String html = R"(
  <!DOCTYPE html>
  <html>
  <head>
    <title>Device Settings</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial, sans-serif; margin: 20px; }
      form { display: grid; gap: 15px; max-width: 500px; }
      .form-group { display: grid; gap: 5px; }
      label { font-weight: bold; }
      input, button, textarea { padding: 10px; }
      button { background: #3498db; color: white; border: none; cursor: pointer; }
      .section { margin-bottom: 20px; padding-bottom: 20px; border-bottom: 1px solid #eee; }
    </style>
  </head>
  <body>
    <h1>Device Settings</h1>
    
    <form action="/update" method="post">
      <div class="section">
        <h2>WiFi Settings</h2>
        <div class="form-group">
          <label>WiFi SSID</label>
          <input type="text" name="wifiSSID" value=")" + config.wifiSSID + R"(">
        </div>
        <div class="form-group">
          <label>WiFi Password</label>
          <input type="password" name="wifiPassword" value=")" + config.wifiPassword + R"(">
        </div>
      </div>
      
      <div class="section">
        <h2>Alert Thresholds</h2>
        <div class="form-group">
          <label>Temp High (°C)</label>
          <input type="number" step="0.1" name="tempHigh" value=")" + String(config.tempHighThreshold) + R"(">
        </div>
        <div class="form-group">
          <label>Temp Low (°C)</label>
          <input type="number" step="0.1" name="tempLow" value=")" + String(config.tempLowThreshold) + R"(">
        </div>
        <div class="form-group">
          <label>Humidity High (%)</label>
          <input type="number" step="0.1" name="humidHigh" value=")" + String(config.humidHighThreshold) + R"(">
        </div>
        <div class="form-group">
          <label>Humidity Low (%)</label>
          <input type="number" step="0.1" name="humidLow" value=")" + String(config.humidLowThreshold) + R"(">
        </div>
      </div>
      
      <div class="section">
        <h2>SMS Settings</h2>
        <div class="form-group">
          <label>SMS Username</label>
          <input type="text" name="smsUser" value=")" + config.smsUsername + R"(">
        </div>
        <div class="form-group">
          <label>SMS Password</label>
          <input type="password" name="smsPass" value=")" + config.smsPassword + R"(">
        </div>
        <div class="form-group">
          <label>Sender Number</label>
          <input type="text" name="smsSender" value=")" + config.smsSender + R"(">
        </div>
        <div class="form-group">
          <label>Recipient Numbers (comma separated)</label>
          <textarea name="smsRecipients">)" + config.smsRecipients + R"(</textarea>
        </div>
      </div>
      
      <div class="section">
        <h2>Admin Credentials</h2>
        <div class="form-group">
          <label>Admin Username</label>
          <input type="text" name="adminUser" value=")" + config.adminUser + R"(">
        </div>
        <div class="form-group">
          <label>Admin Password</label>
          <input type="password" name="adminPass" value=")" + config.adminPass + R"(">
        </div>
      </div>
      
      <button type="submit">Save Settings</button>
    </form>
    
    <p><a href="/">← Back to Dashboard</a></p>
  </body>
  </html>
  )";
  
  server.send(200, "text/html", html);
}

void handleSettingsUpdate() {
  // Update WiFi settings
  config.wifiSSID = server.arg("wifiSSID");
  config.wifiPassword = server.arg("wifiPassword");
  
  // Update thresholds
  config.tempHighThreshold = server.arg("tempHigh").toFloat();
  config.tempLowThreshold = server.arg("tempLow").toFloat();
  config.humidHighThreshold = server.arg("humidHigh").toFloat();
  config.humidLowThreshold = server.arg("humidLow").toFloat();
  
  // Update SMS settings
  config.smsUsername = server.arg("smsUser");
  config.smsPassword = server.arg("smsPass");
  config.smsSender = server.arg("smsSender");
  config.smsRecipients = server.arg("smsRecipients");
  
  // Update admin credentials
  config.adminUser = server.arg("adminUser");
  config.adminPass = server.arg("adminPass");
  
  // Save to persistent storage
  preferences.begin("config", false);
  preferences.putString("wifiSSID", config.wifiSSID);
  preferences.putString("wifiPass", config.wifiPassword);
  preferences.putFloat("tempHigh", config.tempHighThreshold);
  preferences.putFloat("tempLow", config.tempLowThreshold);
  preferences.putFloat("humidHigh", config.humidHighThreshold);
  preferences.putFloat("humidLow", config.humidLowThreshold);
  preferences.putString("smsUser", config.smsUsername);
  preferences.putString("smsPass", config.smsPassword);
  preferences.putString("smsSender", config.smsSender);
  preferences.putString("smsRecipients", config.smsRecipients);
  preferences.putString("adminUser", config.adminUser);
  preferences.putString("adminPass", config.adminPass);
  preferences.end();
  
  // Reconnect to WiFi if credentials changed
  if (WiFi.SSID() != config.wifiSSID) {
    connectToNetwork();
  }
  
  // Redirect to dashboard
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSensorDataAPI() {
  String json = "{";
  json += "\"temperature\":" + String(sensor.temperature, 1) + ",";
  json += "\"humidity\":" + String(sensor.humidity, 1) + ",";
  json += "\"tempHigh\":" + String(config.tempHighThreshold, 1) + ",";
  json += "\"tempLow\":" + String(config.tempLowThreshold, 1) + ",";
  json += "\"humidHigh\":" + String(config.humidHighThreshold, 1) + ",";
  json += "\"humidLow\":" + String(config.humidLowThreshold, 1);
  json += "}";
  
  server.send(200, "application/json", json);
}

// ====================
// Sensor Functions
// ====================
void readSensorData() {
  static unsigned long lastRead = 0;
  const long READ_INTERVAL = 2000;  // 2 seconds
  
  if (millis() - lastRead < READ_INTERVAL) return;
  
  sensor.temperature = dht.readTemperature();
  sensor.humidity = dht.readHumidity();
  
  if (isnan(sensor.temperature)) {
    Serial.println("Failed to read temperature!");
    sensor.temperature = -99.9;
  }
  
  if (isnan(sensor.humidity)) {
    Serial.println("Failed to read humidity!");
    sensor.humidity = -99.9;
  }
  
  lastRead = millis();
  checkAlerts();
  updateLCD();
}

void checkAlerts() {
  // Temperature alert logic
  if (sensor.temperature > config.tempHighThreshold || 
      sensor.temperature < config.tempLowThreshold) {
    if (!sensor.tempAlertActive) {
      sendAlert("Temperature alert: " + String(sensor.temperature, 1) + "°C");
      sensor.tempAlertActive = true;
    }
  } else {
    sensor.tempAlertActive = false;
  }
  
  // Humidity alert logic
  if (sensor.humidity > config.humidHighThreshold || 
      sensor.humidity < config.humidLowThreshold) {
    if (!sensor.humidAlertActive) {
      sendAlert("Humidity alert: " + String(sensor.humidity, 1) + "%");
      sensor.humidAlertActive = true;
    }
  } else {
    sensor.humidAlertActive = false;
  }
}

// ====================
// SMS Functions
// ====================
void sendAlert(String message) {
  if (config.smsUsername == "" || config.smsPassword == "" || 
      config.smsRecipients == "" || config.smsSender == "") {
    Serial.println("SMS not configured. Alert not sent.");
    return;
  }
  
  String soapRequest = R"(
  <soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" 
                   xmlns:tem="http://tempuri.org/">
    <soapenv:Header/>
    <soapenv:Body>
      <tem:SendSimpleSMS>
        <tem:username>)" + config.smsUsername + R"(</tem:username>
        <tem:password>)" + config.smsPassword + R"(</tem:password>
        <tem:to>)" + config.smsRecipients + R"(</tem:to>
        <tem:from>)" + config.smsSender + R"(</tem:from>
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
    Serial.println("Alert SMS sent: " + message);
  } else {
    Serial.println("SMS send failed: " + http.errorToString(httpCode));
  }
  
  http.end();
}

// ====================
// LCD Functions
// ====================
void updateLCD() {
  lcd.clear();
  
  switch (displayMode) {
    case SENSOR_DATA:
      // Line 1: Temperature
      lcd.setCursor(0, 0);
      lcd.print("Temp: ");
      lcd.print(sensor.temperature, 1);
      lcd.print("C");
      
      // Line 2: Humidity
      lcd.setCursor(0, 1);
      lcd.print("Hum: ");
      lcd.print(sensor.humidity, 1);
      lcd.print("%");
      break;
      
    case IP_ADDRESS:
      // Show IP address
      lcd.setCursor(0, 0);
      lcd.print("IP Address:");
      lcd.setCursor(0, 1);
      if (WiFi.status() == WL_CONNECTED) {
        lcd.print(WiFi.localIP().toString());
      } else {
        lcd.print("Not Connected");
      }
      break;
  }
}

void checkButtonPress() {
  static bool lastButtonState = HIGH;
  bool currentButtonState = digitalRead(BUTTON_PIN);
  
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    // Button pressed
    displayMode = IP_ADDRESS;
    updateLCD();
    lastDisplaySwitch = millis();
  }
  
  lastButtonState = currentButtonState;
  
  // Return to sensor display after timeout
  if (displayMode == IP_ADDRESS && 
      millis() - lastDisplaySwitch > DISPLAY_DURATION) {
    displayMode = SENSOR_DATA;
    updateLCD();
  }
}

// ====================
// Main Loop
// ====================
void loop() {
  // Handle web clients
  server.handleClient();
  
  // Read sensor data
  readSensorData();
  
  // Check button press
  checkButtonPress();
  
  // Small delay to prevent watchdog reset
  delay(10);
}