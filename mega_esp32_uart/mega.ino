#include <SPI.h>
#include <Ethernet.h>
#include <DHT.h>
#include <EEPROM.h>

// DHT22 Configuration
#define DHTPIN 2
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Ethernet Configuration
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
EthernetServer server(80);

// Communication with ESP32
#define ESP32_SERIAL Serial1  // TX1=18, RX1=19
#define ACTIVATE_ESP_PIN 3    // ESP32 activation pin

// System variables
unsigned long lastSensorRead = 0;
const long sensorInterval = 2000;  // 2 seconds
bool ethernetConnected = false;
bool espActivated = false;

// Threshold settings structure
struct Thresholds {
  float temp_high;
  float temp_low;
  float hum_high;
  float hum_low;
};

Thresholds currentThresholds = {30.0, 10.0, 80.0, 20.0}; // Default values

// Authentication credentials
const char* www_username = "admin";
const char* www_password = "admin123";

void setup() {
  Serial.begin(115200);
  ESP32_SERIAL.begin(115200);
  pinMode(ACTIVATE_ESP_PIN, OUTPUT);
  digitalWrite(ACTIVATE_ESP_PIN, LOW); // Start with ESP32 disabled
  
  dht.begin();
  
  // Initialize Ethernet
  Ethernet.init(53);  // CS pin for W5500
  if (Ethernet.begin(mac) == 0) {
    Serial.println("Failed to configure Ethernet using DHCP");
    ethernetConnected = false;
    activateESP32(); // Activate ESP32
  } else {
    ethernetConnected = true;
    server.begin();
    Serial.print("Server IP: ");
    Serial.println(Ethernet.localIP());
  }
  
  // Load thresholds from EEPROM
  loadThresholdsFromEEPROM();
}

void loop() {
  // Check Ethernet status every 10 seconds
  static unsigned long lastEthCheck = 0;
  if (millis() - lastEthCheck > 10000) {
    checkEthernet();
    lastEthCheck = millis();
  }

  // Read sensor and manage data
  if (millis() - lastSensorRead >= sensorInterval) {
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    
    if (!isnan(temperature) && !isnan(humidity)) {
      if (ethernetConnected) {
        handleEthernetClients(temperature, humidity);
      } else if (espActivated) {
        sendToESP32(temperature, humidity);
      }
    }
    lastSensorRead = millis();
  }
  
  // Handle serial communication with ESP32
  handleSerialFromESP32();
}

void checkEthernet() {
  if (Ethernet.linkStatus() == LinkON && !ethernetConnected) {
    if (Ethernet.begin(mac) != 0) {
      ethernetConnected = true;
      server.begin();
      Serial.println("Ethernet reconnected");
      deactivateESP32(); // Deactivate ESP32
    }
  } else if (Ethernet.linkStatus() == LinkOFF && ethernetConnected) {
    ethernetConnected = false;
    Serial.println("Ethernet disconnected");
    activateESP32(); // Activate ESP32
  }
}

void activateESP32() {
  if (!espActivated) {
    digitalWrite(ACTIVATE_ESP_PIN, HIGH); // Activate ESP32
    delay(2000); // Wait for ESP32 to boot
    espActivated = true;
    Serial.println("ESP32 activated");
    
    // Send current thresholds to ESP32
    sendThresholdsToESP32();
  }
}

void deactivateESP32() {
  if (espActivated) {
    digitalWrite(ACTIVATE_ESP_PIN, LOW); // Deactivate ESP32
    espActivated = false;
    Serial.println("ESP32 deactivated");
  }
}

void handleEthernetClients(float temp, float hum) {
  EthernetClient client = server.available();
  
  while (client) {
    String request = client.readStringUntil('\r');
    client.flush();
    
    // Check for settings update request
    if (request.indexOf("POST /settings") != -1) {
      handleSettingsUpdate(client, request);
    }
    // Serve main page
    else if (request.indexOf("GET / ") != -1) {
      serveMainPage(client, temp, hum);
    }
    // Serve settings page
    else if (request.indexOf("GET /settings") != -1) {
      if (!authenticate(client)) {
        client.stop();
        return;
      }
      serveSettingsPage(client);
    }
    // Serve default page
    else {
      serveMainPage(client, temp, hum);
    }
    
    client.stop();
  }
}

bool authenticate(EthernetClient client) {
  if (!client.available()) return true;
  
  String authHeader = client.readStringUntil('\n');
  if (authHeader.indexOf("Authorization: Basic") == -1) {
    client.println("HTTP/1.1 401 Unauthorized");
    client.println("WWW-Authenticate: Basic realm=\"Secure Area\"");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();
    client.println("<html><body>Unauthorized</body></html>");
    return false;
  }
  
  String encoded = authHeader.substring(authHeader.indexOf(' ') + 1);
  encoded.trim();
  String decoded = base64Decode(encoded);
  
  if (decoded != String(www_username) + ":" + String(www_password)) {
    client.println("HTTP/1.1 401 Unauthorized");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();
    client.println("<html><body>Invalid credentials</body></html>");
    return false;
  }
  
  return true;
}

void serveMainPage(EthernetClient client, float temp, float hum) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  
  client.println("<html><head>");
  client.println("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  client.println("<style>");
  client.println("body { font-family: Arial; text-align: center; margin-top: 50px; }");
  client.println(".data { font-size: 24px; margin: 20px; }");
  client.println(".value { font-weight: bold; }");
  client.println(".alert { color: red; font-weight: bold; }");
  client.println("</style>");
  client.println("</head><body>");
  client.println("<h1>Environment Monitor</h1>");
  
  // Temperature display with alert
  client.print("<div class='data'>Temperature: <span class='value");
  if (temp > currentThresholds.temp_high || temp < currentThresholds.temp_low) {
    client.print(" alert");
  }
  client.print("'>");
  client.print(temp, 1);
  client.println(" °C</span></div>");
  
  // Humidity display with alert
  client.print("<div class='data'>Humidity: <span class='value");
  if (hum > currentThresholds.hum_high || hum < currentThresholds.hum_low) {
    client.print(" alert");
  }
  client.print("'>");
  client.print(hum, 1);
  client.println(" %</span></div>");
  
  // Display alert messages
  if (temp > currentThresholds.temp_high) {
    client.println("<div class='alert'>High Temperature Warning!</div>");
  }
  else if (temp < currentThresholds.temp_low) {
    client.println("<div class='alert'>Low Temperature Warning!</div>");
  }
  
  if (hum > currentThresholds.hum_high) {
    client.println("<div class='alert'>High Humidity Warning!</div>");
  }
  else if (hum < currentThresholds.hum_low) {
    client.println("<div class='alert'>Low Humidity Warning!</div>");
  }
  
  // Settings link
  client.println("<div style='margin-top: 30px;'><a href='/settings'>Configure Thresholds</a></div>");
  
  client.println("</body></html>");
}

void serveSettingsPage(EthernetClient client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  
  client.println("<html><head>");
  client.println("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  client.println("<style>");
  client.println("body { font-family: Arial; max-width: 500px; margin: 0 auto; padding: 20px; }");
  client.println("h1 { text-align: center; }");
  client.println(".form-group { margin-bottom: 15px; }");
  client.println("label { display: inline-block; width: 200px; }");
  client.println("input { width: 100px; padding: 5px; }");
  client.println("button { padding: 8px 15px; background: #4CAF50; color: white; border: none; }");
  client.println("</style>");
  client.println("</head><body>");
  client.println("<h1>Threshold Settings</h1>");
  client.println("<form action='/settings' method='POST'>");
  
  client.println("<div class='form-group'>");
  client.println("<label for='temp_high'>High Temperature:</label>");
  client.print("<input type='number' step='0.1' name='temp_high' value='");
  client.print(currentThresholds.temp_high, 1);
  client.println("' required> °C");
  client.println("</div>");
  
  client.println("<div class='form-group'>");
  client.println("<label for='temp_low'>Low Temperature:</label>");
  client.print("<input type='number' step='0.1' name='temp_low' value='");
  client.print(currentThresholds.temp_low, 1);
  client.println("' required> °C");
  client.println("</div>");
  
  client.println("<div class='form-group'>");
  client.println("<label for='hum_high'>High Humidity:</label>");
  client.print("<input type='number' step='0.1' name='hum_high' value='");
  client.print(currentThresholds.hum_high, 1);
  client.println("' required> %");
  client.println("</div>");
  
  client.println("<div class='form-group'>");
  client.println("<label for='hum_low'>Low Humidity:</label>");
  client.print("<input type='number' step='0.1' name='hum_low' value='");
  client.print(currentThresholds.hum_low, 1);
  client.println("' required> %");
  client.println("</div>");
  
  client.println("<button type='submit'>Save Settings</button>");
  client.println("</form>");
  client.println("</body></html>");
}

void handleSettingsUpdate(EthernetClient client, String request) {
  if (!authenticate(client)) return;

  // Parse form data
  int tempHighStart = request.indexOf("temp_high=") + 10;
  int tempHighEnd = request.indexOf("&", tempHighStart);
  String tempHighStr = request.substring(tempHighStart, tempHighEnd);
  
  int tempLowStart = request.indexOf("temp_low=") + 9;
  int tempLowEnd = request.indexOf("&", tempLowStart);
  String tempLowStr = request.substring(tempLowStart, tempLowEnd);
  
  int humHighStart = request.indexOf("hum_high=") + 9;
  int humHighEnd = request.indexOf("&", humHighStart);
  String humHighStr = request.substring(humHighStart, humHighEnd);
  
  int humLowStart = request.indexOf("hum_low=") + 8;
  int humLowEnd = request.indexOf(" ", humLowStart);
  String humLowStr = request.substring(humLowStart, humLowEnd);
  
  // Update thresholds
  currentThresholds.temp_high = tempHighStr.toFloat();
  currentThresholds.temp_low = tempLowStr.toFloat();
  currentThresholds.hum_high = humHighStr.toFloat();
  currentThresholds.hum_low = humLowStr.toFloat();
  
  // Save to EEPROM
  saveThresholdsToEEPROM();
  
  // Send response
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<html><body>");
  client.println("<h1>Settings Updated</h1>");
  client.println("<p>Threshold values have been updated successfully.</p>");
  client.println("<a href='/'>Return to Dashboard</a>");
  client.println("</body></html>");
}

void sendToESP32(float temp, float hum) {
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 2000) {
    String jsonData = "{\"t\":" + String(temp, 1) + ",\"h\":" + String(hum, 1) + "}";
    ESP32_SERIAL.println(jsonData);
    lastSend = millis();
  }
}

void sendThresholdsToESP32() {
  String json = "{\"cmd\":\"thresholds\",";
  json += "\"th\":" + String(currentThresholds.temp_high) + ",";
  json += "\"tl\":" + String(currentThresholds.temp_low) + ",";
  json += "\"hh\":" + String(currentThresholds.hum_high) + ",";
  json += "\"hl\":" + String(currentThresholds.hum_low) + "}";
  ESP32_SERIAL.println(json);
}

void handleSerialFromESP32() {
  if (ESP32_SERIAL.available()) {
    String json = ESP32_SERIAL.readStringUntil('\n');
    
    // Threshold update from ESP32
    if (json.indexOf("{\"cmd\":\"thresholds\"") >= 0) {
      parseThresholdUpdate(json);
    }
  }
}

void parseThresholdUpdate(String json) {
  int thStart = json.indexOf("\"th\":") + 4;
  int thEnd = json.indexOf(',', thStart);
  String thStr = json.substring(thStart, thEnd);
  
  int tlStart = json.indexOf("\"tl\":") + 4;
  int tlEnd = json.indexOf(',', tlStart);
  String tlStr = json.substring(tlStart, tlEnd);
  
  int hhStart = json.indexOf("\"hh\":") + 4;
  int hhEnd = json.indexOf(',', hhStart);
  String hhStr = json.substring(hhStart, hhEnd);
  
  int hlStart = json.indexOf("\"hl\":") + 4;
  int hlEnd = json.indexOf('}', hlStart);
  String hlStr = json.substring(hlStart, hlEnd);
  
  currentThresholds.temp_high = thStr.toFloat();
  currentThresholds.temp_low = tlStr.toFloat();
  currentThresholds.hum_high = hhStr.toFloat();
  currentThresholds.hum_low = hlStr.toFloat();
  
  saveThresholdsToEEPROM();
  Serial.println("Thresholds updated from ESP32");
}

void saveThresholdsToEEPROM() {
  EEPROM.put(0, currentThresholds);
}

void loadThresholdsFromEEPROM() {
  Thresholds savedThresholds;
  EEPROM.get(0, savedThresholds);
  
  // Validate loaded values
  if (!isnan(savedThresholds.temp_high) && 
      !isnan(savedThresholds.temp_low) && 
      !isnan(savedThresholds.hum_high) && 
      !isnan(savedThresholds.hum_low)) {
    currentThresholds = savedThresholds;
    Serial.println("Loaded thresholds from EEPROM");
  }
}

// Basic Base64 decoding for authentication
String base64Decode(String input) {
  String decoded = "";
  const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  
  int i = 0;
  int len = input.length();
  while (i < len) {
    uint32_t sextet_a = input[i] == '=' ? 0 & i++ : base64_chars[input[i++]];
    uint32_t sextet_b = input[i] == '=' ? 0 & i++ : base64_chars[input[i++]];
    uint32_t sextet_c = input[i] == '=' ? 0 & i++ : base64_chars[input[i++]];
    uint32_t sextet_d = input[i] == '=' ? 0 & i++ : base64_chars[input[i++]];
    
    uint32_t triple = (sextet_a << 3 * 6) + (sextet_b << 2 * 6) + (sextet_c << 1 * 6) + (sextet_d << 0 * 6);
    
    if (input[i - 2] != '=') decoded += char((triple >> 2 * 8) & 0xFF);
    if (input[i - 1] != '=') decoded += char((triple >> 1 * 8) & 0xFF);
    if (input[i] != '=') decoded += char((triple >> 0 * 8) & 0xFF);
  }
  
  return decoded;
}