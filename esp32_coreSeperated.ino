#include <SPI.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <DHT.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Hardware Configuration
#define ETH_CS_PIN   15
#define ETH_RST_PIN  5
#define ETH_SCK_PIN  14
#define ETH_MISO_PIN 12
#define ETH_MOSI_PIN 13
#define DHTPIN 4
#define DHTTYPE DHT22

// Network Credentials
const char* WIFI_SSID = "iran-smarttech";
const char* WIFI_PASS = "Hh3040650051@";
const char* AP_SSID = "ESP32-SENSOR-AP";
const char* AP_PASS = "password123";

// MAC Address
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};

// Shared Data Structure
struct SharedData {
  float temperature;
  float humidity;
};
SharedData sensorData;
SemaphoreHandle_t dataMutex;

// Network State Flags
volatile bool ethActive = false;
volatile bool wifiActive = false;
volatile bool apActive = false;

// Server Objects
EthernetServer ethServer(80);  // Ethernet web server
WiFiServer wifiServer(80);     // WiFi/AP web server

// Core 0 Task: Ethernet Management
void ethernetTask(void *pvParameters) {
  Serial.println("[C0] Starting Ethernet...");
  
  // W5500 Hardware Reset Sequence
  pinMode(ETH_RST_PIN, OUTPUT);
  digitalWrite(ETH_RST_PIN, HIGH);  // Ensure high first
  delay(100);
  digitalWrite(ETH_RST_PIN, LOW);   // Assert reset
  delay(100);
  digitalWrite(ETH_RST_PIN, HIGH);  // Release reset
  delay(500);                       // Stabilization period
  
  // Initialize SPI and Ethernet
  SPI.begin(ETH_SCK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
  Ethernet.init(ETH_CS_PIN);
  
  // Attempt Ethernet connection with DHCP
  if (Ethernet.begin(mac, 10000)) {  // 10s timeout
    Serial.print("[C0] Ethernet connected! IP: ");
    Serial.println(Ethernet.localIP());
    
    // Disable WiFi completely
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    
    ethActive = true;
    ethServer.begin();
    
    // Main Ethernet processing loop
    while (true) {
      // Handle incoming clients
      EthernetClient client = ethServer.available();
      if (client) {
        handleHTTPRequest(client);
        client.stop();
      }
      
      // Maintain DHCP lease
      Ethernet.maintain();
      vTaskDelay(1);
    }
  } else {
    Serial.println("[C0] Ethernet connection failed");
    
    // Detailed error diagnostics
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      Serial.println("[C0] Hardware not detected");
    } else if (Ethernet.linkStatus() == LinkOFF) {
      Serial.println("[C0] Cable disconnected");
    } else {
      Serial.println("[C0] DHCP request failed");
    }
    
    // Full hardware reset
    digitalWrite(ETH_RST_PIN, LOW);
    Ethernet.init(255);  // Invalidate CS pin
    SPI.end();
  }
  
  vTaskDelete(NULL);
}

// Core 1 Task: WiFi/AP and Sensor Management
void wifiSensorTask(void *pvParameters) {
  Serial.println("[C1] Starting sensor and network fallback...");
  
  // Initialize sensor
  DHT dht(DHTPIN, DHTTYPE);
  dht.begin();
  
  // Wait for Ethernet outcome (11s)
  vTaskDelay(11000 / portTICK_PERIOD_MS);
  
  // Activate WiFi if Ethernet failed
  if (!ethActive) {
    Serial.println("[C1] Starting WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    // Connection attempt with timeout
    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      vTaskDelay(500 / portTICK_PERIOD_MS);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("\n[C1] WiFi connected! IP: ");
      Serial.println(WiFi.localIP());
      wifiActive = true;
      wifiServer.begin();
    } else {
      Serial.println("\n[C1] WiFi failed! Starting AP...");
      WiFi.softAP(AP_SSID, AP_PASS);
      Serial.print("[C1] AP IP: ");
      Serial.println(WiFi.softAPIP());
      apActive = true;
      wifiServer.begin();  // Use same server for AP mode
    }
  }
  
  // Main sensor and network loop
  unsigned long lastSensorTime = 0;
  while (true) {
    // Read sensor every 2 seconds
    if (millis() - lastSensorTime >= 2000) {
      float temp = dht.readTemperature();
      float humi = dht.readHumidity();
      
      // Update shared data with protection
      if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        if (!isnan(temp)) sensorData.temperature = temp;
        if (!isnan(humi)) sensorData.humidity = humi;
        xSemaphoreGive(dataMutex);
      }
      lastSensorTime = millis();
    }
    
    // Handle WiFi/AP clients if active
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

// Unified HTTP Request Handler
void handleHTTPRequest(Client &client) {
  // Read request headers
  String request = "";
  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      request += c;
      if (request.endsWith("\r\n\r\n")) break;
    }
  }
  
  // Only respond to root requests
  if (request.indexOf("GET / ") != -1 || request.indexOf("GET / HTTP") != -1) {
    // Get sensor data safely
    float temp, humi;
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      temp = sensorData.temperature;
      humi = sensorData.humidity;
      xSemaphoreGive(dataMutex);
    }
    
    // Generate HTML response
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Sensor Monitor</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='2'>";
    html += "<style>body{font-family:Arial,sans-serif;text-align:center;}</style>";
    html += "</head><body>";
    html += "<h1>Environment Monitor</h1>";
    html += "<div style='display:inline-block;margin:10px;padding:20px;border:1px solid #ccc;'>";
    html += "<h2>Temperature</h2><p style='font-size:24px;color:red;'>" + String(temp,1) + " °C</p></div>";
    html += "<div style='display:inline-block;margin:10px;padding:20px;border:1px solid #ccc;'>";
    html += "<h2>Humidity</h2><p style='font-size:24px;color:blue;'>" + String(humi,1) + " %</p></div>";
    html += "<p>Connection: ";
    if (ethActive) html += "Ethernet";
    else if (wifiActive) html += "WiFi";
    else if (apActive) html += "Access Point";
    html += "</p></body></html>";
    
    // Send HTTP response
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();
    client.println(html);
  }
  else {
    // Handle 404 for other paths
    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("404 Not Found");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Initialize mutex for shared data
  dataMutex = xSemaphoreCreateMutex();
  
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
    12000,           // Stack size
    NULL,            // Parameters
    1,               // Priority
    NULL,            // Task handle
    1                // Core 1
  );
}

void loop() {
  // FreeRTOS takes over - no need for loop content
  vTaskDelete(NULL);
}
