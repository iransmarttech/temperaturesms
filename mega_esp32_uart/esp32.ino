#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// WiFi Configuration
const char* ssid = "iran-smarttech";
const char* password = "Hh3040650051@";
const char* ap_ssid = "ESP32_AP";
const char* ap_password = "12341234";

// Web Server
WebServer server(80);

// Serial Communication with Mega
#define MEGA_SERIAL Serial2  // RX2=16, TX2=17

// Sensor data and thresholds
float currentTemp = 0.0;
float currentHum = 0.0;
unsigned long lastDataUpdate = 0;

struct Thresholds {
  float temp_high;
  float temp_low;
  float hum_high;
  float hum_low;
};

Thresholds currentThresholds = {30.0, 10.0, 80.0, 20.0}; // Default values

// Authentication
const char* www_username = "admin";
const char* www_password = "admin123";

// Preferences for NVS storage
Preferences preferences;

// HTML Content (Static) - Updated with alert system
const char* htmlContent = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>IoT Sensor</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {font-family: Arial; text-align: center; margin-top: 50px;}
    .data {font-size: 24px; margin: 20px;}
    .value {font-weight: bold; color: #2e7d32;}
    .alert {color: red; font-weight: bold; margin: 10px 0; padding: 10px; border: 1px solid red; border-radius: 5px;}
    .settings-form {max-width: 500px; margin: 0 auto; padding: 20px; text-align: left;}
    .form-group {margin-bottom: 15px;}
    label {display: inline-block; width: 200px;}
    input {width: 100px; padding: 5px;}
    button {padding: 8px 15px; background: #4CAF50; color: white; border: none;}
  </style>
  <script>
    function updateData() {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          // Update values
          document.getElementById('temp').innerText = data.t.toFixed(1);
          document.getElementById('hum').innerText = data.h.toFixed(1);
          
          // Update alerts
          const alertContainer = document.getElementById('alerts');
          alertContainer.innerHTML = '';
          
          if(data.t > data.thresholds.temp_high) {
            alertContainer.innerHTML += '<div class="alert">High Temperature Warning! (' + data.t.toFixed(1) + '°C)</div>';
          }
          if(data.t < data.thresholds.temp_low) {
            alertContainer.innerHTML += '<div class="alert">Low Temperature Warning! (' + data.t.toFixed(1) + '°C)</div>';
          }
          if(data.h > data.thresholds.hum_high) {
            alertContainer.innerHTML += '<div class="alert">High Humidity Warning! (' + data.h.toFixed(1) + '%)</div>';
          }
          if(data.h < data.thresholds.hum_low) {
            alertContainer.innerHTML += '<div class="alert">Low Humidity Warning! (' + data.h.toFixed(1) + '%)</div>';
          }
        })
        .catch(error => console.error('Error:', error));
    }
    setInterval(updateData, 2000);
    
    function saveSettings() {
      const formData = new FormData(document.getElementById('settings-form'));
      fetch('/settings', {
        method: 'POST',
        body: formData
      })
      .then(response => {
        if(response.ok) {
          alert('Settings saved successfully!');
          updateData();
        } else {
          alert('Error saving settings');
        }
      });
      return false;
    }
  </script>
</head>
<body>
  <h1>Environment Monitor</h1>
  
  <div id="alerts"></div>
  
  <div class="data">
    Temperature: <span id="temp" class="value">--</span> °C
  </div>
  <div class="data">
    Humidity: <span id="hum" class="value">--</span> %
  </div>
  
  <div style="margin-top: 30px;">
    <a href="/settings">Configure Thresholds</a>
  </div>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  MEGA_SERIAL.begin(115200, SERIAL_8N1, 16, 17);  // RX=16, TX=17

  // Load thresholds from NVS
  preferences.begin("thresholds", false);
  currentThresholds.temp_high = preferences.getFloat("t_high", 30.0);
  currentThresholds.temp_low = preferences.getFloat("t_low", 10.0);
  currentThresholds.hum_high = preferences.getFloat("h_high", 80.0);
  currentThresholds.hum_low = preferences.getFloat("h_low", 20.0);
  preferences.end();

  // Connect to WiFi or start AP
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi...");
  
  if (WiFi.waitForConnectResult(10000) != WL_CONNECTED) {
    Serial.println("\nFailed to connect. Starting AP...");
    WiFi.softAP(ap_ssid, ap_password);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("\nConnected! IP: ");
    Serial.println(WiFi.localIP());
  }

  // Server routes
  server.on("/", HTTP_GET, [](){
    server.send(200, "text/html", htmlContent);
  });
  
  server.on("/data", HTTP_GET, [](){
    char jsonData[150];
    snprintf(jsonData, sizeof(jsonData), 
             "{\"t\":%.2f,\"h\":%.2f,\"thresholds\":{\"temp_high\":%.2f,\"temp_low\":%.2f,\"hum_high\":%.2f,\"hum_low\":%.2f}}", 
             currentTemp, currentHum, 
             currentThresholds.temp_high, currentThresholds.temp_low,
             currentThresholds.hum_high, currentThresholds.hum_low);
    server.send(200, "application/json", jsonData);
  });
  
  server.on("/settings", HTTP_GET, [](){
    if(!server.authenticate(www_username, www_password)) {
      return server.requestAuthentication();
    }
    serveSettingsPage();
  });
  
  server.on("/settings", HTTP_POST, [](){
    if(!server.authenticate(www_username, www_password)) {
      return server.requestAuthentication();
    }
    handleSettingsUpdate();
  });

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  readFromMega();
}

void readFromMega() {
  if (MEGA_SERIAL.available()) {
    String json = MEGA_SERIAL.readStringUntil('\n');
    json.trim();
    
    Serial.print("Raw JSON: ");
    Serial.println(json);

    // Check for threshold update command
    if (json.indexOf("{\"cmd\":\"thresholds\"") >= 0) {
      parseThresholdUpdate(json);
    } 
    // Otherwise parse sensor data
    else {
      parseSensorData(json);
    }
  }
  
  // Reset data if no update for 10 seconds
  if (millis() - lastDataUpdate > 10000) {
    currentTemp = 0.0;
    currentHum = 0.0;
  }
}

void parseSensorData(String json) {
  StaticJsonDocument<100> doc;
  DeserializationError error = deserializeJson(doc, json);
  
  if(error) {
    Serial.print("JSON Error: ");
    Serial.println(error.c_str());
    return;
  }
  
  currentTemp = doc["t"];
  currentHum = doc["h"];
  lastDataUpdate = millis();
  Serial.printf("Parsed: %.2f°C, %.2f%%\n", currentTemp, currentHum);
}

void parseThresholdUpdate(String json) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, json);
  
  if(error) {
    Serial.print("Threshold JSON Error: ");
    Serial.println(error.c_str());
    return;
  }
  
  currentThresholds.temp_high = doc["th"];
  currentThresholds.temp_low = doc["tl"];
  currentThresholds.hum_high = doc["hh"];
  currentThresholds.hum_low = doc["hl"];
  
  saveThresholdsToNVS();
  Serial.println("Thresholds updated from Mega");
}

void serveSettingsPage() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta charset="UTF-8">
    <title>Threshold Settings</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body {font-family: Arial; text-align: center; margin-top: 50px;}
      .settings-form {max-width: 500px; margin: 0 auto; padding: 20px; text-align: left;}
      .form-group {margin-bottom: 15px;}
      label {display: inline-block; width: 200px;}
      input {width: 100px; padding: 5px;}
      button {padding: 8px 15px; background: #4CAF50; color: white; border: none;}
    </style>
  </head>
  <body>
    <h1>Threshold Settings</h1>
    <form id="settings-form" class="settings-form" onsubmit="return saveSettings()">
      <div class="form-group">
        <label for="temp_high">High Temperature:</label>
        <input type="number" step="0.1" name="temp_high" value=")rawliteral";
  html += String(currentThresholds.temp_high, 1);
  html += R"rawliteral(" required> °C
      </div>
      
      <div class="form-group">
        <label for="temp_low">Low Temperature:</label>
        <input type="number" step="0.1" name="temp_low" value=")rawliteral";
  html += String(currentThresholds.temp_low, 1);
  html += R"rawliteral(" required> °C
      </div>
      
      <div class="form-group">
        <label for="hum_high">High Humidity:</label>
        <input type="number" step="0.1" name="hum_high" value=")rawliteral";
  html += String(currentThresholds.hum_high, 1);
  html += R"rawliteral(" required> %
      </div>
      
      <div class="form-group">
        <label for="hum_low">Low Humidity:</label>
        <input type="number" step="0.1" name="hum_low" value=")rawliteral";
  html += String(currentThresholds.hum_low, 1);
  html += R"rawliteral(" required> %
      </div>
      
      <button type="submit">Save Settings</button>
    </form>
    <script>
      function saveSettings() {
        const formData = new FormData(document.getElementById('settings-form'));
        fetch('/settings', {
          method: 'POST',
          body: formData
        })
        .then(response => {
          if(response.ok) {
            alert('Settings saved successfully!');
          } else {
            alert('Error saving settings');
          }
        });
        return false;
      }
    </script>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

void handleSettingsUpdate() {
  // Get new values from form
  float new_temp_high = server.arg("temp_high").toFloat();
  float new_temp_low = server.arg("temp_low").toFloat();
  float new_hum_high = server.arg("hum_high").toFloat();
  float new_hum_low = server.arg("hum_low").toFloat();

  // Update thresholds
  currentThresholds.temp_high = new_temp_high;
  currentThresholds.temp_low = new_temp_low;
  currentThresholds.hum_high = new_hum_high;
  currentThresholds.hum_low = new_hum_low;
  
  // Save to NVS
  saveThresholdsToNVS();
  
  // Send updated thresholds to Mega
  sendThresholdsToMega();
  
  // Send success response
  server.sendHeader("Location", "/");
  server.send(303);
}

void saveThresholdsToNVS() {
  preferences.begin("thresholds", false);
  preferences.putFloat("t_high", currentThresholds.temp_high);
  preferences.putFloat("t_low", currentThresholds.temp_low);
  preferences.putFloat("h_high", currentThresholds.hum_high);
  preferences.putFloat("h_low", currentThresholds.hum_low);
  preferences.end();
  Serial.println("Thresholds saved to NVS");
}

void sendThresholdsToMega() {
  String json = "{\"cmd\":\"thresholds\",";
  json += "\"th\":" + String(currentThresholds.temp_high) + ",";
  json += "\"tl\":" + String(currentThresholds.temp_low) + ",";
  json += "\"hh\":" + String(currentThresholds.hum_high) + ",";
  json += "\"hl\":" + String(currentThresholds.hum_low) + "}";
  
  MEGA_SERIAL.println(json);
  Serial.println("Sent thresholds to Mega: " + json);
}