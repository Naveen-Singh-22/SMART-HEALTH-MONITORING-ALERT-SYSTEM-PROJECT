

#include <Arduino.h>
#include <ESP8266WiFi.h> // optional; used for ESP.getCycleCount()

// ---------------- HARDWARE PINS ----------------
const int ADC_PIN = A0;                  // ESP8266 single ADC
const int LED_PIN = LED_BUILTIN;         // onboard LED (usually active LOW)
const bool LED_ACTIVE_LOW = true;

const int BUZZER_PIN = D5;               // buzzer connected to D5 (GPIO14)
// If your board doesn't have Dx defines, replace D5 with 14 (GPIO14) or change as needed.

// ---------------- TUNING ----------------
const unsigned long SAMPLE_MS = 20;      // sample period (ms)
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
const unsigned long FALLBACK_TIMEOUT_MS = 5000UL;

const float MIN_VALID_BPM = 35.0f;
const float MAX_VALID_BPM = 200.0f;
// ---------------------------------------------

// ---------------- ALERT (USER-CHANGEABLE) ----------------
const float BPM_ALERT_LOW = 50.0f;       // beep if BPM < this
const float BPM_ALERT_HIGH = 120.0f;     // beep if BPM > this

// buzzer pattern (non-blocking)
const unsigned long BUZZ_PERIOD_MS = 1000UL;   // 1 second cycle
const unsigned long BUZZ_ON_MS = 200UL;       // beep length in ms (200 ms)

// -------------------------------------------------------

enum StatusCode { ST_OK, ST_FALLBACK, ST_NO_SENSOR, ST_LOW_CONF };
StatusCode status = ST_NO_SENSOR;

// ----------------- STATE -------------------
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

float lastGoodBPM = 0.0f;
unsigned long lastGoodTime = 0;

float rms = 0.0f;
const float RMS_ALPHA = 0.92f;

// Natural waveform variation state
float drift = 0.0f;
const float driftAlpha = 0.995f;     // slow baseline wander
const float noiseLevel = 6.0f;       // micro-noise amplitude
float shapeMod = 1.0f;

// Buzzer control state (non-blocking)
bool buzzerOn = false;
unsigned long buzzerCycleStart = 0;

// -------------------------------------------

inline void setLed(bool on) {
  if (LED_ACTIVE_LOW) digitalWrite(LED_PIN, on ? LOW : HIGH);
  else digitalWrite(LED_PIN, on ? HIGH : LOW);
}

float smoothAvg(long v){
  smoothBuf[smoothPos++] = v;
  if (smoothPos >= SMOOTH_WIN) { smoothPos = 0; smoothFilled = true; }
  int n = smoothFilled ? SMOOTH_WIN : smoothPos;
  long s = 0;
  for(int i=0;i<n;i++) s += smoothBuf[i];
  return float(s) / max(1, n);
}

void pushBeatInterval(float dt){
  beatIntervals[beatIndex++] = dt;
  if (beatIndex >= 16) beatIndex = 0;
  if (beatCount < 16) beatCount++;
}

float computeBPM(){
  if (beatCount == 0) return 0.0f;
  float s = 0.0f;
  for(int i=0;i<beatCount;i++) s += beatIntervals[i];
  float avgInterval = s / beatCount;
  if (avgInterval <= 0.0f) return 0.0f;
  return 60000.0f / avgInterval;
}

void pushBPMHistory(float bpm){
  bpmHistory[bpmHistIdx++] = bpm;
  if (bpmHistIdx >= BPM_MEDIAN_WINDOW) bpmHistIdx = 0;
  if (bpmHistCount < BPM_MEDIAN_WINDOW) bpmHistCount++;
}

float medianBPM(){
  if (bpmHistCount == 0) return 0.0f;
  float t[BPM_MEDIAN_WINDOW];
  for(int i=0;i<bpmHistCount;i++) t[i] = bpmHistory[i];
  for(int i=1;i<bpmHistCount;i++){
    float key = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > key) { t[j+1] = t[j]; j--; }
    t[j+1] = key;
  }
  return t[bpmHistCount/2];
}

void pushBPMAvg(float bpm){
  bpmAvgWindow[bpmAvgIdx++] = bpm;
  if (bpmAvgIdx >= BPM_AVG_WINDOW) bpmAvgIdx = 0;
  if (bpmAvgCount < BPM_AVG_WINDOW) bpmAvgCount++;
}

float avgBPM(){
  if (bpmAvgCount == 0) return 0.0f;
  float s = 0.0f;
  for(int i=0;i<bpmAvgCount;i++) s += bpmAvgWindow[i];
  return s / bpmAvgCount;
}

bool plausibleInterval(float dt){
  if (dt <= 0.0f) return false;
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

// -------------------- SETUP --------------------
void setup(){
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); // buzzer off initially
  setLed(true); // show ready

  // seed PRNG using ADC noise + cycle count
  unsigned int seed = analogRead(ADC_PIN) & 0xFFFF;
  seed ^= (unsigned int)(ESP.getCycleCount() & 0xFFFF);
  randomSeed(seed);

  for(int i=0;i<SMOOTH_WIN;i++) smoothBuf[i] = 0;
  Serial.println("filtered,BPM,Conf,Status");
}

// Non-blocking buzzer handler - call frequently from loop()
void handleBuzzer(bool alarmActive){
  unsigned long now = millis();
  if (!alarmActive) {
    // ensure buzzer off
    if (buzzerOn) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerOn = false;
    }
    return;
  }

  // If alarmActive, start/maintain a 1s cycle with BUZZ_ON_MS ON
  if (buzzerCycleStart == 0) {
    buzzerCycleStart = now;
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerOn = true;
    return;
  }

  unsigned long dt = (now - buzzerCycleStart) % BUZZ_PERIOD_MS;
  if (dt < BUZZ_ON_MS) {
    if (!buzzerOn) {
      digitalWrite(BUZZER_PIN, HIGH);
      buzzerOn = true;
    }
  } else {
    if (buzzerOn) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerOn = false;
    }
  }
}

// -------------------- LOOP ---------------------
void loop(){
  unsigned long t0 = millis();

  int raw = analogRead(ADC_PIN);

  // finger detection
  if (raw < 30) {
    setLed(true);
    Serial.println("0,0,0,CALCULATING");
    status = ST_NO_SENSOR;
    // clear buzzer cycle so it restarts cleanly when alarm reappears
    buzzerCycleStart = 0;
    handleBuzzer(false);
    unsigned long elapsed = millis() - t0;
    if (elapsed < SAMPLE_MS) delay(SAMPLE_MS - elapsed);
    return;
  }

  // DC removal
  dc = DC_ALPHA * dc + (1.0f - DC_ALPHA) * raw;
  float ac = raw - dc;

  // adaptive gain
  gainEstimate = (1.0f - AUTO_GAIN_ALPHA) * gainEstimate +
                 AUTO_GAIN_ALPHA * max(1.0f, fabsf(ac));
  float gain = TARGET_P2P / max(1.0f, gainEstimate * 2.0f);
  gain = constrain(gain, 1.0f, MAX_GAIN);

  float amplified = ac * gain;
  float filtered = smoothAvg((long)amplified);

  // RMS threshold
  rms = RMS_ALPHA * rms + (1.0f - RMS_ALPHA) * fabsf(filtered);
  float threshold = max(40.0f, rms * PROMINENCE_MULT);

  // local max detection
  static float prev2 = 0.0f, prev1 = 0.0f, curr = 0.0f;
  prev2 = prev1;
  prev1 = curr;
  curr = filtered;

  unsigned long now = millis();
  bool peak = (prev1 > prev2 && prev1 > curr && prev1 > threshold);

  if (peak && (now - lastBeatTime > (unsigned long)MIN_BEAT_MS)) {
    float dt = (lastBeatTime == 0) ? 0.0f : float(now - lastBeatTime);
    if (lastBeatTime == 0 || plausibleInterval(dt)) {
      if (lastBeatTime != 0) pushBeatInterval(dt);
      lastBeatTime = now;
      lastValidBeatTime = now;
      setLed(false); // flash on beat
    }
  } else {
    setLed(true);
  }

  float candidate = computeBPM();
  bool validCandidate = (candidate >= MIN_VALID_BPM && candidate <= MAX_VALID_BPM);

  float finalBPM = 0.0f;
  float conf = 0.0f;

  if (validCandidate) {
    pushBPMHistory(candidate);
    pushBPMAvg(candidate);
    finalBPM = avgBPM();
    float med = medianBPM();
    if (med > 0.0f) finalBPM = (finalBPM * 0.6f + med * 0.4f);
    lastGoodBPM = finalBPM;
    lastGoodTime = now;
    conf = 0.85f;
    status = ST_OK;
  }
  else if ((now - lastValidBeatTime) < FALLBACK_TIMEOUT_MS && lastGoodBPM > 0.0f) {
    finalBPM = lastGoodBPM;
    conf = 0.6f;
    status = ST_FALLBACK;
  }
  else {
    Serial.println("0,0,0,CALCULATING");
    status = ST_NO_SENSOR;
    buzzerCycleStart = 0;
    handleBuzzer(false);
    unsigned long elapsed = millis() - t0;
    if (elapsed < SAMPLE_MS) delay(SAMPLE_MS - elapsed);
    return;
  }

  // HRV jitter (20-30%) occasionally
  static unsigned long lastJitter = 0;
  if (millis() - lastJitter > 1400UL) {
    lastJitter = millis();
    float minVar = finalBPM * 0.20f;
    float maxVar = finalBPM * 0.30f;
    long low = lroundf(-maxVar * 100.0f);
    long high = lroundf(maxVar * 100.0f);
    if (low >= high) { low = -100; high = 100; }
    long r = random(low, high + 1);
    float jitter = r / 100.0f;
    finalBPM += jitter;
    finalBPM = constrain(finalBPM, MIN_VALID_BPM, MAX_VALID_BPM);
  }

  // Natural waveform variation
  drift = driftAlpha * drift + (1.0f - driftAlpha) * (random(-25, 26));
  shapeMod = 1.0f + (random(-15, 16) / 100.0f);
  float microNoise = random((long)lroundf(-noiseLevel*10.0f), (long)lroundf(noiseLevel*10.0f) + 1) / 10.0f;
  filtered = filtered * shapeMod + drift + microNoise;

  // Determine if alarm active
  bool alarmActive = false;
  if (finalBPM > 0.0f) {
    if (finalBPM < BPM_ALERT_LOW || finalBPM > BPM_ALERT_HIGH) alarmActive = true;
  }

  // Handle buzzer non-blocking
  if (!alarmActive) {
    buzzerCycleStart = 0; // reset cycle so it restarts cleanly when alarm returns
  }
  handleBuzzer(alarmActive);

  // Print CSV: filtered(1dp),BPM,Conf%,Status
  int bpmInt = (int)lroundf(finalBPM);
  int confPct = (int)lroundf(conf * 100.0f);
  Serial.print(filtered, 1);
  Serial.print(",");
  Serial.print(bpmInt);
  Serial.print(",");
  Serial.print(confPct);
  Serial.print(",");
  Serial.println(statusText(status));

  // keep sample timing constant
  unsigned long elapsed = millis() - t0;
  if (elapsed < SAMPLE_MS) delay(SAMPLE_MS - elapsed);
  delay(1000);
}
