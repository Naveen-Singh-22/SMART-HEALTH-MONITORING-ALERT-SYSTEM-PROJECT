/*
Smart Health Monitoring & Alert System (NodeMCU ESP8266)
Sensors: MAX30102 (SpO2 + Pulse), DS18B20 (Temperature)
Cloud & Alerts: Blynk only (notifications + virtual pins)
- MAX30102: SDA -> D2 (GPIO4), SCL -> D1 (GPIO5)
- DS18B20: Data -> D5 (GPIO14) (use 4.7k pull-up if module doesn't have it)
Libraries needed: Wire, MAX30105 (or MAX30102-compatible), heartRate helper,
OneWire, DallasTemperature, BlynkSimpleEsp8266, ESP8266WiFi
*/
#define BLYNK_PRINT Serial
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <OneWire.h>
#include <DallasTemperature.h>
// Use SparkFun MAX30105 library (MAX30102 compatible)
#include "MAX30105.h"
#include "heartRate.h"   // helper functions (from SparkFun examples)
// ========== USER CONFIG ==========
char WIFI_SSID[] = "YOUR_SSID";
char WIFI_PASS[] = "YOUR_PASS";
char BLYNK_AUTH[] = "YOUR_BLYNK_TOKEN";

// Blynk virtual pins - map these in the Blynk app
#define V_BPM      V1    // BPM value display
#define V_SPO2     V2    // SpO2 display
#define V_TEMP     V3    // Temperature display
#define V_ALERT    V4    // General alert text
#define V_ALERT_CARETAKER V5 // Alert specifically for caretaker dashboard
#define V_ALERT_DOCTOR    V6 // Alert specifically for doctor dashboard
#define V_STATUS   V7    // Device status (online/last seen)
// ========== PINS ==========
#define PIN_MAX_SDA D2 // GPIO4
#define PIN_MAX_SCL D1 // GPIO5
#define PIN_DS18B20 D5 // GPIO14
// ========== THRESHOLDS (tweak as needed) ==========
const float SPO2_THRESHOLD = 92.0;     // below => alert
const int   BPM_LOW = 45;              // below => bradycardia alert
const int   BPM_HIGH = 130;            // above => tachycardia alert
const float TEMP_THRESHOLD = 38.0;     // degrees Celsius => fever alert
// ========== ALERT RATE-LIMITING (seconds) ==========
const unsigned long ALERT_MIN_INTERVAL = 60UL; // minimum seconds between same alert to same recipient
const unsigned long SUMMARY_INTERVAL = 300UL;  // send a periodic summary at most every 5 minutes
// ========== SENSORS & LIBRARIES ==========
MAX30105 particleSensor;
OneWire oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);
// Buffers for MAX30102
const int BUFFER_SIZE = 100;
uint32_t redBuffer[BUFFER_SIZE];
uint32_t irBuffer[BUFFER_SIZE];
int bufferIndex = 0;
// Computed values
volatile int bpmValue = 0;
volatile int spo2Value = 0;
volatile bool bpmValid = false;
volatile bool spo2Valid = false;
// timing
unsigned long lastUploadTime = 0;
const unsigned long UPLOAD_INTERVAL = 10000UL; // 10 seconds for dashboard updates
// alert timing trackers
unsigned long lastCaretakerAlert = 0;
unsigned long lastDoctorAlert = 0;
unsigned long lastSummarySend = 0;
// Blynk timer
BlynkTimer timer;
// ============================================================
// Helper: Safe string building (small utility)
String buildAlertText(const String &who, const String &what) {
String s = "[" + who + "] " + what;
return s;
}
// ============================================================
// Read temperature (DS18B20)
float readTemperatureC() {
tempSensor.requestTemperatures();
float t = tempSensor.getTempCByIndex(0);
if (t == DEVICE_DISCONNECTED_C) return NAN;
return t;
}
// ============================================================
// Read MAX30102: fill a buffer of samples then compute HR & SPO2
// Using SparkFun style helpers for BPM; SPO2 approximated here but can be replaced with a library
void sampleMAX30102() {
// fill buffers
int i = 0;
while (i < BUFFER_SIZE) {
if (particleSensor.available()) {
redBuffer[i] = particleSensor.getRed();
irBuffer[i]  = particleSensor.getIR();
i++;
particleSensor.nextSample();
} else {
delay(5);
}
}
// compute HR using a helper (peak detection)
int32_t hr = getHeartRate(irBuffer, BUFFER_SIZE);
if (hr > 30 && hr < 220) {
bpmValue = hr; bpmValid = true;
} else {
bpmValid = false;
}
// crude SPO2 estimate (placeholder) - replace with valid algorithm for production
long sr = 0, sir = 0;
for (int j = 0; j < BUFFER_SIZE; ++j) {
sr += redBuffer[j];
sir += irBuffer[j];
}
if (sir > 0) {
float ratio = float(sr) / float(sir);
int approxSpO2 = int(constrain(110.0 - 25.0 * ratio, 50, 100));
spo2Value = approxSpO2; spo2Valid = true;
} else {
spo2Valid = false;
}
}
// ============================================================
// Alert dispatch logic - sends alerts to caretaker and doctor separately.
// Uses rate limiting and different messages for each.
void dispatchAlertsIfNeeded(int bpm, int spo2, float tempC) {
unsigned long now = millis() / 1000UL; // seconds
String summary = "";
bool isCritical = false;
// Build issues list
if (spo2Valid && spo2 < SPO2_THRESHOLD) {
summary += "Low SpO2: " + String(spo2) + "%; ";
isCritical = true;
}
if (bpmValid && (bpm < BPM_LOW || bpm > BPM_HIGH)) {
summary += "Abnormal BPM: " + String(bpm) + " bpm; ";
isCritical = true;
}
if (!isnan(tempC) && tempC > TEMP_THRESHOLD) {
summary += "High Temp: " + String(tempC,1) + " C; ";
isCritical = true;
}
if (!isCritical) return; // nothing to alert
// Prepare messages (more detailed for doctor)
String caretakerMsg = "Alert: " + summary + "Please check the patient.";
String doctorMsg = "URGENT: " + summary + "Suggest immediate review. Readings: BPM="
+ String(bpmValid?bpm:-1) + " SpO2=" + String(spo2Valid?spo2:-1)
+ " Temp=" + (isnan(tempC) ? String("N/A") : String(tempC,1)) + "C";
// Caretaker alert (app push + virtual pin)
if (now - lastCaretakerAlert >= ALERT_MIN_INTERVAL) {
// push notification to any Blynk user registered to this project
Blynk.notify(caretakerMsg);
// write to caretaker-specific virtual pin (so caretaker's dashboard widget can show it)
Blynk.virtualWrite(V_ALERT_CARETAKER, caretakerMsg);
lastCaretakerAlert = now;
}

// Doctor alert (more strict rate-limiting)
if (now - lastDoctorAlert >= (ALERT_MIN_INTERVAL * 2)) {
// Additionally write the detailed message to doctor's virtual pin
Blynk.virtualWrite(V_ALERT_DOCTOR, doctorMsg);
// Send a general push too (doctors often have the same Blynk app)
Blynk.notify(doctorMsg); // note: this sends to all project users. To target only doctor, create separate project or use integration.
lastDoctorAlert = now;
}
// Write general alert for main dashboard (V_ALERT)
Blynk.virtualWrite(V_ALERT, summary);
}
// ============================================================
// Periodic upload: read sensors, compute, update Blynk displays and possibly send alerts
void periodicTask() {
// 1) Read temperature
float tC = readTemperatureC();

// 2) Sample MAX30102 & compute HR/SPO2
sampleMAX30102();

// 3) Debug serial
Serial.print("BPM: ");
if (bpmValid) Serial.print(bpmValue); else Serial.print("N/A");
Serial.print("  SpO2: ");
if (spo2Valid) Serial.print(spo2Value); else Serial.print("N/A");
Serial.print("  Temp: ");
if (!isnan(tC)) Serial.print(tC,1); else Serial.print("N/A");
Serial.println();
// 4) Update Blynk widgets
if (bpmValid) Blynk.virtualWrite(V_BPM, bpmValue); else Blynk.virtualWrite(V_BPM, 0);
if (spo2Valid) Blynk.virtualWrite(V_SPO2, spo2Value); else Blynk.virtualWrite(V_SPO2, 0);
if (!isnan(tC)) Blynk.virtualWrite(V_TEMP, tC); else Blynk.virtualWrite(V_TEMP, 0);
// 5) Dispatch alerts intelligently
dispatchAlertsIfNeeded(bpmValue, spo2Value, tC);
// 6) Periodic summary (less frequent)
unsigned long now = millis() / 1000UL;
if (now - lastSummarySend >= SUMMARY_INTERVAL) {
String state = "OK";
if (!bpmValid || !spo2Valid || isnan(tC)) state = "Sensor/Read Error";
Blynk.virtualWrite(V_STATUS, "Last summary: " + state + " at " + String(now));
lastSummarySend = now;
}
}
// ============================================================
// Setup and loop
void setup() {
Serial.begin(115200);
delay(50);
Serial.println("Starting Smart Health Monitor...");

// Blynk + WiFi setup
WiFi.mode(WIFI_STA);
Blynk.begin(BLYNK_AUTH, WIFI_SSID, WIFI_PASS); // Blynk will block here until connected (or until timeout depending on lib version)

// Sensors init
tempSensor.begin();

Wire.begin(); // I2C

if (!particleSensor.begin(Wire)) {
Serial.println("MAX3010x not found. Check wiring/power.");
} else {
Serial.println("MAX3010x found.");
particleSensor.setup(); // default settings
particleSensor.setPulseAmplitudeRed(0x0A);
particleSensor.setPulseAmplitudeIR(0x0A);
}
// set timer: periodicTask runs every 10 seconds (adjustable)
timer.setInterval(10000L, periodicTask);

// run timer in background
timer.run();
}

void loop() {
Blynk.run();
timer.run();
}
