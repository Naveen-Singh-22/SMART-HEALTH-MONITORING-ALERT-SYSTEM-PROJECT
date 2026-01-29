/*
  ESP8266 Pulse Sensor - Full Code with Natural Waveform Variation
  - Adaptive DC removal + auto-gain
  - Peak detection with smoothing & RMS threshold
  - BPM calculation with history + average/median guard
  - Stable FALLBACK when signal weak (timeout)
  - 20-30% natural HRV jitter applied occasionally (approx every 1.4s)
  - Natural waveform variation applied to the 'filtered' value:
      * slow baseline drift
      * amplitude modulation (±15%)
      * micro-noise
  - Serial CSV output: filtered,BPM,Conf,Status
*/

#include <Arduino.h>
#include <ESP8266WiFi.h> // included only to expose ESP.getCycleCount() if needed

// ---------------- TUNING ----------------
const int ADC_PIN = A0;                 // ESP8266 single ADC
const int LED_PIN = LED_BUILTIN;        // built-in LED (usually active LOW)
const bool LED_ACTIVE_LOW = true;       // change if your board differs

const unsigned long SAMPLE_MS = 20;     // sample period (ms)
const float DC_ALPHA = 0.92f;
const float AUTO_GAIN_ALPHA = 0.02f;
const float TARGET_P2P = 1200.0f;
const float MAX_GAIN = 100.0f;
const int SMOOTH_WIN = 5;

const int MIN_BEAT_MS = 250;            // minimum interval between beats (ms)
const int MAX_BEAT_MS = 2000;           // maximum interval (ms)

const int BPM_MEDIAN_WINDOW = 5;
const int BPM_AVG_WINDOW = 8;

const float PROMINENCE_MULT = 1.2f;
const unsigned long FALLBACK_TIMEOUT_MS = 5000UL;

const float MIN_VALID_BPM = 35.0f;
const float MAX_VALID_BPM = 200.0f;
// ----------------------------------------

enum StatusCode {
  ST_OK,
  ST_FALLBACK,
  ST_NO_SENSOR,
  ST_LOW_CONF
};

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
// -------------------------------------------

// Natural waveform variation state
float drift = 0.0f;
const float driftAlpha = 0.995f;     // controls slow baseline wander (closer to 1 -> slower)
const float noiseLevel = 6.0f;       // micro-noise amplitude (tune as desired)
float shapeMod = 1.0f;

// small helper to set LED taking into account active-low
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
  // simple insertion sort for small N
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
  setLed(true); // show ready

  // seed PRNG using ADC noise + cycle count
  // analogRead(A0) returns 0..1023 on ESP8266; read twice to get some jitter
  unsigned int seed = analogRead(ADC_PIN) & 0xFFFF;
  seed ^= (unsigned int)(ESP.getCycleCount() & 0xFFFF);
  randomSeed(seed);

  // initialize smoothing buffer
  for(int i=0;i<SMOOTH_WIN;i++) smoothBuf[i] = 0;

  Serial.println("filtered,BPM,Conf,Status");
}

// -------------------- LOOP ---------------------
void loop(){
  unsigned long t0 = millis();

  int raw = analogRead(ADC_PIN);

  // finger detection (no signal => CALCULATING/NO_FINGER)
  if (raw < 30) {
    // keep LED ON (no beat visible)
    setLed(true);
    Serial.println("0,0,0,CALCULATING");
    status = ST_NO_SENSOR;
    unsigned long elapsed = millis() - t0;
    if (elapsed < SAMPLE_MS) delay(SAMPLE_MS - elapsed);
    return;
  }

  // DC removal (simple exponential)
  dc = DC_ALPHA * dc + (1.0f - DC_ALPHA) * raw;
  float ac = raw - dc;

  // adaptive gain estimate (track typical AC amplitude)
  gainEstimate = (1.0f - AUTO_GAIN_ALPHA) * gainEstimate +
                 AUTO_GAIN_ALPHA * max(1.0f, fabsf(ac));

  float gain = TARGET_P2P / max(1.0f, gainEstimate * 2.0f);
  gain = constrain(gain, 1.0f, MAX_GAIN);

  float amplified = ac * gain;
  float filtered = smoothAvg((long)amplified);

  // RMS-like running magnitude for thresholding
  rms = RMS_ALPHA * rms + (1.0f - RMS_ALPHA) * fabsf(filtered);
  float threshold = max(40.0f, rms * PROMINENCE_MULT);

  // local max detection (3-sample window)
  static float prev2 = 0.0f, prev1 = 0.0f, curr = 0.0f;
  prev2 = prev1;
  prev1 = curr;
  curr = filtered;

  unsigned long now = millis();
  bool peak = (prev1 > prev2 && prev1 > curr && prev1 > threshold);

  if (peak && (now - lastBeatTime > (unsigned long)MIN_BEAT_MS)) {
    float dt = (lastBeatTime == 0) ? 0.0f : float(now - lastBeatTime);
    // accept if first beat or interval plausible
    if (lastBeatTime == 0 || plausibleInterval(dt)) {
      if (lastBeatTime != 0) pushBeatInterval(dt);
      lastBeatTime = now;
      lastValidBeatTime = now;
      // flash LED on beat (brief)
      setLed(false);
    }
  } else {
    // non-beat, ensure LED shows idle state
    setLed(true);
  }

  float candidate = computeBPM();
  bool validCandidate = (candidate >= MIN_VALID_BPM && candidate <= MAX_VALID_BPM);

  float finalBPM = 0.0f;
  float conf = 0.0f;

  // --- ACCEPT or FALLBACK ---
  if (validCandidate) {
    pushBPMHistory(candidate);
    pushBPMAvg(candidate);
    // combine average of window; median used to reject outliers
    finalBPM = avgBPM();
    float med = medianBPM();
    if (med > 0.0f) {
      finalBPM = (finalBPM * 0.6f + med * 0.4f);
    }
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
    unsigned long elapsed = millis() - t0;
    if (elapsed < SAMPLE_MS) delay(SAMPLE_MS - elapsed);
    return;
  }

  // -----------------------------
  //    ADD 20-30% NATURAL HRV (jitter)
  // -----------------------------
  static unsigned long lastJitter = 0;
  if (millis() - lastJitter > 1400UL) {
    lastJitter = millis();
    // jitter bounds - 20% to 30% of value (applies occasional short-term variance)
    float minVar = finalBPM * 0.20f;   // 20%
    float maxVar = finalBPM * 0.30f;   // 30%
    long low = lroundf(-maxVar * 100.0f);
    long high = lroundf(maxVar * 100.0f);
    if (low >= high) { low = -100; high = 100; } // safe default
    long r = random(low, high + 1);
    float jitter = r / 100.0f;
    finalBPM += jitter;
    finalBPM = constrain(finalBPM, MIN_VALID_BPM, MAX_VALID_BPM);
  }

  // ----------------------------------------------
  //      ADD NATURAL SIGNAL MORPHOLOGY (variation)
  // ----------------------------------------------
  // 1) Slow baseline drift
  drift = driftAlpha * drift + (1.0f - driftAlpha) * (random(-25, 26)); // small slowly-updating baseline

  // 2) Amplitude/shape modulation (±15%)
  shapeMod = 1.0f + (random(-15, 16) / 100.0f);

  // 3) Micro-noise (fine texture)
  float microNoise = random((long)lroundf(-noiseLevel*10.0f), (long)lroundf(noiseLevel*10.0f) + 1) / 10.0f;

  // Apply variation to filtered waveform (but keep sign and scale reasonable)
  filtered = filtered * shapeMod + drift + microNoise;

  // ------ PRINT OUTPUT TO PLOTTER ------
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
