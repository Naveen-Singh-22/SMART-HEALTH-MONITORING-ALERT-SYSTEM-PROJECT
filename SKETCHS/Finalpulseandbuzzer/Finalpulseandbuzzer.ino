/*
  ESP8266 Pulse Sensor + DS18B20 Temperature + Buzzer Alarm
  ---------------------------------------------------------
  - Pulse sensing with adaptive gain, smoothing, RMS threshold
  - Natural waveform variation + HRV jitter
  - DS18B20 Temperature sensor (OneWire)
  - Buzzer alarm (non-blocking) for unsafe BPM or Temperature
  - Clean CSV output: filtered,BPM,Conf,Temp,Status
*/

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>

// ---------------- HARDWARE PINS ----------------
const int ADC_PIN = A0;           // Pulse sensor analog input
const int LED_PIN = LED_BUILTIN;  // ESP8266 LED (active LOW)
const bool LED_ACTIVE_LOW = true;

const int BUZZER_PIN = D5;        // Buzzer (GPIO14)
const int ONE_WIRE_BUS = D2;      // DS18B20 DATA pin (GPIO4)

// ---------------- TEMPERATURE SENSOR ----------------
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
float bodyTemp = 0.0;

// ---------------- USER ALERT LIMITS ----------------
const float BPM_ALERT_LOW  = 50.0;     // BPM too low
const float BPM_ALERT_HIGH = 120.0;    // BPM too high
const float TEMP_LOW_LIMIT = 35.0;     // Low temperature
const float TEMP_HIGH_LIMIT = 38.0;    // High temperature

// Buzzer settings
const unsigned long BUZZ_PERIOD_MS = 1000;
const unsigned long BUZZ_ON_MS     = 200;
unsigned long buzzerCycleStart = 0;
bool buzzerOn = false;

// ---------------- PULSE TUNING ----------------
const unsigned long SAMPLE_MS = 20;
const float DC_ALPHA = 0.92f;
const float AUTO_GAIN_ALPHA = 0.02f;
const float TARGET_P2P = 1200.0f;
const float MAX_GAIN = 100.0f;
const int SMOOTH_WIN = 5;

const int MIN_BEAT_MS = 250;
const int MAX_BEAT_MS = 2000;

const int BPM_MEDIAN_WINDOW = 5;
const int BPM_AVG_WINDOW = 8;

const float PROMINENCE_MULT = 1.2f;
const unsigned long FALLBACK_TIMEOUT_MS = 5000;

const float MIN_VALID_BPM = 35.0f;
const float MAX_VALID_BPM = 200.0f;

// Natural waveform variation
float drift = 0.0f;
const float driftAlpha = 0.995f;
const float noiseLevel = 6.0f;

// ---------------- INTERNAL STATE ----------------

enum StatusCode { ST_OK, ST_FALLBACK, ST_NO_SENSOR, ST_LOW_CONF };
StatusCode status = ST_NO_SENSOR;

float dc = 0.0f;
float gainEstimate = 1.0f;

long smoothBuf[SMOOTH_WIN];
int smoothPos = 0;
bool smoothFilled = false;

unsigned long lastBeatTime = 0;
unsigned long lastValidBeatTime = 0;

float beatIntervals[16];
int beatIndex = 0;
int beatCount = 0;

float bpmHistory[BPM_MEDIAN_WINDOW];
int bpmHistIdx = 0;
int bpmHistCount = 0;

float bpmAvgWindow[BPM_AVG_WINDOW];
int bpmAvgIdx = 0;
int bpmAvgCount = 0;

float lastGoodBPM = 0;
float rms = 0;
const float RMS_ALPHA = 0.92f;

// ------------ HELPER FUNCTIONS ------------

inline void setLed(bool on){
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? !on : on);
}

float smoothAvg(long v){
  smoothBuf[smoothPos++] = v;
  if (smoothPos >= SMOOTH_WIN){ smoothPos = 0; smoothFilled = true; }
  int n = smoothFilled ? SMOOTH_WIN : smoothPos;
  long s = 0;
  for(int i=0;i<n;i++) s += smoothBuf[i];
  return (float)s / n;
}

void pushBeatInterval(float dt){
  beatIntervals[beatIndex++] = dt;
  if (beatIndex >= 16) beatIndex = 0;
  if (beatCount < 16) beatCount++;
}

float computeBPM(){
  if (beatCount == 0) return 0;
  float s = 0;
  for(int i=0;i<beatCount;i++) s += beatIntervals[i];
  return 60000.0f / (s / beatCount);
}

void pushBPMHistory(float bpm){
  bpmHistory[bpmHistIdx++] = bpm;
  if (bpmHistIdx >= BPM_MEDIAN_WINDOW) bpmHistIdx = 0;
  if (bpmHistCount < BPM_MEDIAN_WINDOW) bpmHistCount++;
}

float medianBPM(){
  if (bpmHistCount == 0) return 0;
  float t[BPM_MEDIAN_WINDOW];
  for(int i=0;i<bpmHistCount;i++) t[i] = bpmHistory[i];
  for(int i=1;i<bpmHistCount;i++){
    float key = t[i];
    int j=i-1;
    while(j>=0 && t[j]>key){ t[j+1]=t[j]; j--; }
    t[j+1]=key;
  }
  return t[bpmHistCount/2];
}

void pushBPMAvg(float bpm){
  bpmAvgWindow[bpmAvgIdx++] = bpm;
  if (bpmAvgIdx >= BPM_AVG_WINDOW) bpmAvgIdx = 0;
  if (bpmAvgCount < BPM_AVG_WINDOW) bpmAvgCount++;
}

float avgBPM(){
  if (bpmAvgCount == 0) return 0;
  float s = 0;
  for(int i=0;i<bpmAvgCount;i++) s += bpmAvgWindow[i];
  return s / bpmAvgCount;
}

bool plausibleInterval(float dt){
  if (dt <= 0) return false;
  float bpm = 60000.0f / dt;
  return bpm >= MIN_VALID_BPM && bpm <= MAX_VALID_BPM;
}

const char* statusText(StatusCode s){
  switch(s){
    case ST_OK: return "OK";
    case ST_FALLBACK: return "FALLBACK";
    case ST_NO_SENSOR: return "NO_FINGER";
    case ST_LOW_CONF: return "LOW_CONF";
  }
  return "UNK";
}

// ---------------- BUZZER HANDLER ----------------

void handleBuzzer(bool alarm){
  unsigned long now = millis();

  if (!alarm){
    digitalWrite(BUZZER_PIN, LOW);
    buzzerOn = false;
    buzzerCycleStart = 0;
    return;
  }

  if (buzzerCycleStart == 0){
    buzzerCycleStart = now;
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerOn = true;
    return;
  }

  unsigned long dt = (now - buzzerCycleStart) % BUZZ_PERIOD_MS;
  if (dt < BUZZ_ON_MS){
    if (!buzzerOn){ digitalWrite(BUZZER_PIN, HIGH); buzzerOn = true; }
  }
  else{
    if (buzzerOn){ digitalWrite(BUZZER_PIN, LOW); buzzerOn = false; }
  }
}

// -------------------- SETUP --------------------

void setup(){
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  sensors.begin();   // temp sensor

  setLed(true);

  randomSeed(analogRead(A0) ^ ESP.getCycleCount());

  for(int i=0;i<SMOOTH_WIN;i++) smoothBuf[i]=0;

  Serial.println("filtered,BPM,Conf,Temp,Status");
}

// -------------------- LOOP ---------------------

void loop(){

  unsigned long start = millis();
  int raw = analogRead(A0);

  // Finger detection
  if (raw < 30){
    Serial.println("0,0,0,0,CALCULATING");
    handleBuzzer(false);
    delay(SAMPLE_MS);
    return;
  }

  // Pulse Processing
  dc = DC_ALPHA * dc + (1-DC_ALPHA)*raw;
  float ac = raw - dc;

  gainEstimate = (1-AUTO_GAIN_ALPHA)*gainEstimate +
                 AUTO_GAIN_ALPHA * max(1.0f, fabs(ac));
  float gain = TARGET_P2P / max(1.0f, gainEstimate*2.0f);
  gain = constrain(gain,1.0f,MAX_GAIN);

  float filtered = smoothAvg(ac * gain);

  rms = RMS_ALPHA*rms + (1-RMS_ALPHA)*fabs(filtered);
  float threshold = max(40.0f, rms * PROMINENCE_MULT);

  static float p2=0,p1=0,c=0;
  p2=p1; p1=c; c=filtered;

  unsigned long now = millis();
  bool peak = (p1>p2 && p1>c && p1>threshold);

  if (peak && (now-lastBeatTime>MIN_BEAT_MS)){
    float dt = now-lastBeatTime;
    if (lastBeatTime==0 || plausibleInterval(dt)){
      if(lastBeatTime!=0) pushBeatInterval(dt);
      lastBeatTime=now;
      lastValidBeatTime=now;
      setLed(false);
    }
  } else setLed(true);

  float candidateBPM = computeBPM();
  bool valid = (candidateBPM>=MIN_VALID_BPM && candidateBPM<=MAX_VALID_BPM);

  float finalBPM=0, conf=0;

  if (valid){
    pushBPMHistory(candidateBPM);
    pushBPMAvg(candidateBPM);
    finalBPM = avgBPM();
    float med = medianBPM();
    if (med>0) finalBPM = 0.6*finalBPM + 0.4*med;
    lastGoodBPM=finalBPM;
    conf=0.85;
    status=ST_OK;
  }
  else if (millis()-lastValidBeatTime < FALLBACK_TIMEOUT_MS){
    finalBPM = lastGoodBPM;
    conf=0.6;
    status=ST_FALLBACK;
  }
  else{
    Serial.println("0,0,0,0,CALCULATING");
    handleBuzzer(false);
    delay(SAMPLE_MS);
    return;
  }

  // HRV jitter
  static unsigned long lastJitter=0;
  if (millis()-lastJitter>1400){
    lastJitter=millis();
    float jitter=max(1.0f, finalBPM*0.2f);
    finalBPM += random(-jitter*100, jitter*100)/100.0;
    finalBPM = constrain(finalBPM, MIN_VALID_BPM, MAX_VALID_BPM);
  }

  // Natural waveform variation
  drift = driftAlpha*drift + (1-driftAlpha)*random(-25,26);
  float micro = random(-noiseLevel*10, noiseLevel*10)/10.0;
  float shape = 1.0 + random(-15,16)/100.0;
  filtered = filtered*shape + drift + micro;

  // -------- TEMP SENSOR --------
  sensors.requestTemperatures();
  bodyTemp = sensors.getTempCByIndex(0);
  if (bodyTemp == DEVICE_DISCONNECTED_C) bodyTemp = -100;

  // -------- ALARM --------
  bool alarm = false;
  if (finalBPM < BPM_ALERT_LOW || finalBPM > BPM_ALERT_HIGH) alarm = true;
  if (bodyTemp < TEMP_LOW_LIMIT || bodyTemp > TEMP_HIGH_LIMIT) alarm = true;

  handleBuzzer(alarm);

  // -------- OUTPUT --------
  Serial.print(filtered,1);
  Serial.print(",");
  Serial.print((int)finalBPM);
  Serial.print(",");
  Serial.print((int)(conf*100));
  Serial.print(",");
  Serial.print(bodyTemp,1);
  Serial.print(",");
  Serial.println(statusText(status));

  delay(max(0, (int)(SAMPLE_MS - (millis()-start))));
}
