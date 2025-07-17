# Environment Monitoring System

This ESP32-based project monitors temperature and humidity, displays readings on an LCD, and sends SMS alerts when thresholds are exceeded. It also provides a web interface for configuration.

## Hardware Connections

### Required Components:
- ESP32 Development Board
- DHT22 Temperature/Humidity Sensor
- 16x2 I2C LCD Display
- Push Button
- Breadboard and jumper wires

### Pin Connections:

| Component | ESP32 Pin |
|-----------|-----------|
| DHT22 Data | GPIO 4 |
| LCD SDA | GPIO 21 |
| LCD SCL | GPIO 22 |
| Button | GPIO 5 |

Connect all components to 3.3V power and ground.

## Setup Instructions:
1. Connect hardware as specified above
2. Upload the provided code to ESP32
3. Connect to the device's WiFi network
4. Access web interface at `http://[ESP32-IP]`
5. Configure settings and alert thresholds

Default admin credentials: admin/admin

## Features:
- Real-time temperature/humidity monitoring
- Web-based configuration panel
- SMS alerts for threshold violations
- LCD display of readings and IP address
- Persistent configuration storage
