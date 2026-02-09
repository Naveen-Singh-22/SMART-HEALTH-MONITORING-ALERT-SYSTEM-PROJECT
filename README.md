# Smart Health Monitoring & Alert System

A comprehensive IoT-based health monitoring system built on NodeMCU ESP8266 that continuously monitors vital signs and sends intelligent alerts to caregivers and doctors through the Blynk cloud platform.

---

## 1. Introduction to the Project Working

The Smart Health Monitoring & Alert System Using IoT is designed to **continuously measure key health parameters** such as heart rate, body temperature, and blood oxygen saturation (SpO₂). The system collects this data through biomedical sensors attached to the user. These values are read by a **NodeMCU ESP8266 microcontroller**, which processes the measurements and sends them to cloud platforms like Blynk and ThingSpeak through Wi-Fi.

The **real-time data can be viewed on a mobile dashboard or web interface**, letting caregivers and doctors observe vital signs instantly. If any measured value crosses its safe threshold—for example, low SpO₂ or high temperature—the system automatically triggers alerts. These alerts can be delivered through **push notifications, email, or SMS**, ensuring immediate action.

This project **reduces the need for repeated hospital visits**, especially for elderly patients or people with chronic illnesses, and provides a **reliable, low-cost, real-time monitoring solution** that works anytime and anywhere.

---

## Overview

This project creates a real-time health monitoring solution that:
- **Continuously monitors** blood oxygen (SpO2), heart rate (BPM), and body temperature
- **Sends intelligent alerts** to both caregivers and doctors based on predefined thresholds
- **Uploads data** to the cloud via Blynk for remote monitoring
- **Implements rate-limiting** to prevent alert fatigue
- **Provides separate dashboards** for different user roles (caretaker, doctor)

---

## Features

✅ **Real-time Health Monitoring**
- Blood Oxygen (SpO2) levels
- Heart Rate (BPM) measurement
- Body Temperature monitoring

✅ **Intelligent Alert System**
- Automatic detection of critical health conditions
- Separate notifications for caretakers and doctors
- Rate-limiting to prevent alert spam
- Customizable alert thresholds

✅ **Cloud Integration**
- Blynk cloud platform integration
- Remote monitoring via mobile/web dashboard
- Push notifications to registered users
- Virtual pins for real-time data display

✅ **Multi-user Support**
- Role-based alerts (caretaker vs. doctor)
- Different messaging for different user types
- Detailed logs for medical professionals

---

## Hardware Requirements

### Microcontroller
- **NodeMCU ESP8266** (main processing unit)
- Aurduino uno can be used.

### Sensors
- **MAX30102** - Optical pulse oximeter & heart rate sensor
  - Measures blood oxygen (SpO2) and pulse rate (BPM)
  - Communication: I2C

- **DS18B20** - Digital temperature sensor
  - Measures body temperature
  - Communication: One-Wire protocol
  - Recommended: Use 4.7kΩ pull-up resistor if module doesn't have one

### Wiring

```
MAX30102:
  SDA → D2 (GPIO4)
  SCL → D1 (GPIO5)
  GND → GND
  VCC → 3.3V

DS18B20:
  Data Pin → D5 (GPIO14)
  GND → GND
  VCC → 3.3V
  (Add 4.7kΩ pull-up between Data and VCC if needed)
```

---

## Software Requirements

### Arduino Libraries
Install these libraries via Arduino IDE → Sketch → Include Library → Manage Libraries:

1. **BlynkSimpleEsp8266** - Blynk integration
2. **ESP8266WiFi** - WiFi connectivity (built-in)
3. **Wire** - I2C communication (built-in)
4. **OneWire** - DS18B20 communication
5. **DallasTemperature** - DS18B20 temperature reading
6. **MAX30105** - MAX30102 sensor driver (SparkFun library)
7. **heartRate** - Heart rate calculation helper (from SparkFun MAX30105 examples)

### Cloud Service
- **Blynk Account** (free tier available at [blynk.cloud](https://blynk.cloud))

---

## Installation & Setup

### 1. Arduino IDE Configuration

1. Install Arduino IDE (or use PlatformIO)
2. Add NodeMCU ESP8266 board support:
   - File → Preferences → Additional Boards Manager URLs
   - Add: `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
   - Tools → Board → Boards Manager → Search "ESP8266" → Install

### 2. Install Required Libraries

Use Arduino IDE Library Manager or command line:
```bash
# Via Arduino IDE: Sketch → Include Library → Manage Libraries
# Search and install each library listed above
```

### 3. Configure WiFi and Blynk Credentials

Edit the **Final_Code.ino** file and update these lines:

```cpp
char WIFI_SSID[] = "YOUR_SSID";           // Your WiFi network name
char WIFI_PASS[] = "YOUR_PASS";           // Your WiFi password
char BLYNK_AUTH[] = "YOUR_BLYNK_TOKEN";   // Your Blynk authentication token
```

To get your Blynk token:
1. Sign up at [blynk.cloud](https://blynk.cloud)
2. Create a new device/template
3. Copy the authentication token from device settings

### 4. Upload the Code

1. Connect NodeMCU to your computer via USB
2. Select: Tools → Board → NodeMCU 1.0 (ESP-12E)
3. Select appropriate COM port
4. Click Upload (→)

---

## Configuration & Thresholds

Adjust these constants in **Final_Code.ino** to customize alert behavior:

```cpp
// Alert thresholds (modify as needed)
const float SPO2_THRESHOLD = 92.0;     // Alert if SpO2 below this value (%)
const int   BPM_LOW = 45;              // Alert if BPM below this (bradycardia)
const int   BPM_HIGH = 130;            // Alert if BPM above this (tachycardia)
const float TEMP_THRESHOLD = 38.0;     // Alert if temperature above this (°C)

// Alert timing
const unsigned long ALERT_MIN_INTERVAL = 60UL;    // Minimum 60 seconds between same alerts
const unsigned long SUMMARY_INTERVAL = 300UL;     // Send summary every 5 minutes
const unsigned long UPLOAD_INTERVAL = 10000UL;    // Update dashboard every 10 seconds
```

---

## Blynk Dashboard Setup

### Virtual Pins Mapping

Configure these virtual pins in your Blynk app:

| Virtual Pin | Purpose | Widget Type |
|---|---|---|
| V1 (V_BPM) | Heart Rate Display | Gauge / Label |
| V2 (V_SPO2) | Blood Oxygen Display | Gauge / Label |
| V3 (V_TEMP) | Temperature Display | Gauge / Label |
| V4 (V_ALERT) | General Alert Messages | Label / Notification |
| V5 (V_ALERT_CARETAKER) | Caretaker-specific Alerts | Label / Notification |
| V6 (V_ALERT_DOCTOR) | Doctor-specific Alerts | Label / Notification |
| V7 (V_STATUS) | Device Status | Label |

### Creating the Dashboard

1. Open Blynk app on your phone
2. Create a new template/device
3. Switch to "Edit" mode
4. Add widgets for each virtual pin
5. Assign each widget to the corresponding virtual pin
6. Set up push notifications in app settings

---

## How It Works

### Data Flow

```
Sensors → NodeMCU → Processing → Alert Logic → Blynk Cloud → Users
  ↓         ↓           ↓             ↓            ↓
MAX30102   Read &      Compute    Check        Push
DS18B20    Store    HR, SpO2     Thresholds   Notifications
                    Temperature
```

### Alert Logic

1. **Data Reading**: Sensors are sampled every 10 seconds
2. **Threshold Check**: Values compared against configured limits
3. **Alert Dispatch**:
   - If critical condition detected → send alerts
   - Caretaker gets general alert with quick instructions
   - Doctor gets detailed alert with all readings
4. **Rate Limiting**: Same alert type won't repeat within min interval
5. **Dashboard Update**: All values sent to Blynk app

---

## Serial Monitor Output

When connected via USB, the serial monitor displays:

```
Starting Smart Health Monitor...
MAX3010x found.
BPM: 75  SpO2: 98  Temp: 36.5
BPM: 76  SpO2: 97  Temp: 36.5
Alert: Low SpO2: 91%; Please check the patient.
...
```

Monitor this to debug sensor readings and verify operation.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| MAX30102 not found | Check I2C wiring (SDA/SCL pins), verify pull-up resistors, try I2C scanner sketch |
| DS18B20 showing NAN | Check one-wire connection to D5, verify 4.7kΩ pull-up resistor |
| No WiFi connection | Verify SSID and password are correct, check WiFi signal strength |
| Blynk not connecting | Verify authentication token is correct, check internet connection |
| No alerts being sent | Check threshold values, verify Blynk app notifications are enabled |
| Spurious readings | Allow 30 seconds for sensors to stabilize after power-on |

---

## Project Structure

```
SMART HEALTH MONITORING & ALERT SYSTEM/
├── Final_Code/
│   └── Final_Code.ino              # Main firmware code
├── PROJECT CODE/
│   ├── BLOOD OXYGEN AND BPM.txt    # Reference code
│   ├── Body temp.txt               # Reference code
│   ├── BUZZER TEST CODE.txt        # Optional buzzer integration
│   └── SKETCHS/                    # Development sketches
│       ├── BPM_BLOOD_OXYGEN/
│       ├── DIGITAL_TEMPERATURE/
│       └── ... (other test sketches)
├── diagram.json                    # Wokwi simulation diagram
├── wokwi.toml                      # Wokwi configuration
└── README.md                       # This file
```

---

## Performance Specifications

- **Update Interval**: 10 seconds (customizable)
- **SpO2 Accuracy**: ±2% (MAX30102 spec)
- **BPM Accuracy**: ±5 bpm (MAX30102 spec)
- **Temperature Accuracy**: ±0.5°C (DS18B20 spec)
- **WiFi Range**: Up to 100m (typical)
- **Power Consumption**: ~100mA (WiFi active)

---

## Future Enhancements

🔜 Add SD card logging for historical data
🔜 Implement advanced SpO2 algorithm for better accuracy
🔜 Add local buzzer alerts
🔜 Battery backup with low-power mode
🔜 Multiple patient monitoring from single hub
🔜 Email notifications
🔜 Mobile app with trend analysis
🔜 Machine learning for anomaly detection

---

## Licensing & Attribution

This project uses libraries from:
- **SparkFun** (MAX30105 library, heartRate helper)
- **Blynk** (cloud platform)
- **Dallas Semiconductor** (DallasTemperature library)

---

## Author

**Naveen Singh**  
Smart Health Monitoring & Alert System Project

---

## Support & Contact

For issues, questions, or improvements:
1. Check the Troubleshooting section above
2. Review serial monitor output for error messages
3. Verify all wiring connections
4. Test sensors individually with provided sketches

---
# 🩺 Smart Health Monitoring & Alert System

![Project Banner](assets/images/hardware.jpg)

A comprehensive IoT-based health monitoring system built on NodeMCU ESP8266 that continuously monitors vital signs and sends intelligent alerts to caregivers and doctors.

---

## 📸 Project Demonstration

### 🔧 Hardware Setup
![Hardware Setup](assets/images/hardware.jpg)

### 📱 Blynk Dashboard
![Blynk Dashboard](assets/images/dashboard.png)

### 💻 Serial Monitor Output
![Serial Output](assets/images/serial.png)

### 🧩 Wiring Diagram
![Wiring Diagram](assets/images/wiring.png)

---

## 📖 Introduction

The Smart Health Monitoring & Alert System Using IoT is designed to continuously measure key health parameters such as heart rate, body temperature, and blood oxygen saturation (SpO₂). The system uses biomedical sensors connected to a NodeMCU ESP8266 and uploads data to the cloud for remote monitoring.

If abnormal values are detected, automatic alerts are sent to caregivers and doctors for immediate response.

---

## 🚀 Overview

- Continuous monitoring of SpO₂, BPM, and temperature  
- Intelligent alert system with rate limiting  
- Cloud-based dashboard using Blynk  
- Multi-user access (caretaker & doctor)

---

## ✨ Features

### ✅ Real-time Monitoring
- SpO₂, Heart Rate, Temperature

### ✅ Smart Alerts
- Critical condition detection  
- Role-based notifications  
- Custom thresholds  

### ✅ Cloud Integration
- Blynk platform  
- Mobile & Web dashboard  

### ✅ Multi-user System
- Caretaker & Doctor access  
- Medical logs  

---

## 🔧 Hardware Requirements

### Microcontroller
- NodeMCU ESP8266  
- Arduino Uno (optional)

### Sensors
- MAX30102 (SpO₂ & BPM)  
- DS18B20 (Temperature)
  

---

## Disclaimer

⚠️ **This is a prototype system and should NOT be used as a primary medical device.** Always consult with healthcare professionals for actual medical diagnosis and treatment. This system is for monitoring and educational purposes only.

---

**Last Updated**: January 2026  
**Version**: 1.0
