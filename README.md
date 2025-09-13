# Environment Monitoring System

This ESP32-based project monitors temperature and humidity, displays readings on an LCD, provides web interface access, and sends SMS alerts when thresholds are exceeded.

## Hardware Connections

### Required Components:
- ESP32 Development Board
- DHT22 Temperature/Humidity Sensor
- 16x2 I2C LCD Display
- W5500 Ethernet Module
- Push Button
- Buzzer, LED, and Relay
- Breadboard and jumper wires

### Pin Connections:

| Component | ESP32 Pin |
|-----------|-----------|
| DHT22 Data | GPIO 4 |
| LCD SDA | GPIO 21 |
| LCD SCL | GPIO 22 |
| Button | GPIO 25 |
| Buzzer | GPIO 26 |
| LED | GPIO 27 |
| Relay | GPIO 32 |
| Ethernet CS | GPIO 15 |
| Ethernet RST | GPIO 5 |
| Ethernet SCK | GPIO 14 |
| Ethernet MISO | GPIO 12 |
| Ethernet MOSI | GPIO 13 |

Connect all components to 3.3V power and ground.

## Setup Instructions:
1. Connect hardware as specified above
2. Upload the provided code to ESP32
3. The device will attempt Ethernet connection first, then fall back to WiFi or AP mode
4. Access web interface at `http://[ESP32-IP]` or `http://temperature-vru.local`
5. Configure settings and alert thresholds

Default admin credentials: admin/password

## Features:
- Real-time temperature/humidity monitoring
- Web-based configuration panel with dark theme
- SMS alerts for threshold violations
- LCD display of readings and IP address
- Multi-network support (Ethernet, WiFi, Access Point)
- mDNS support for easy network discovery
- Persistent configuration storage
