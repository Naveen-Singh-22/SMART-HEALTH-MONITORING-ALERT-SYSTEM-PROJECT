

#include <Arduino.h>

// ---------------- TUNING ----------------
const int ADC_PIN = A0;
const int LED_PIN = LED_BUILTIN;

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
const float FALLBACK_TIMEOUT_MS = 5000;

const float MIN_VALID_BPM = 35.0f;
const float MAX_VALID_BPM = 200.0f;
// ----------------------------------------


// ----------- FIX ENUM (NO CONFLICT) -----------
enum StatusCode {
  ST_OK,
  ST_FALLBACK,
  ST_NO_SENSOR,
  ST_LOW_CONF
};

StatusCode status = ST_NO_SENSOR;
// ----------------------------------------------


float dc = 0.0f;
float gainEstimate = 1.0f;

long smoothBuf[SMOOTH_WIN];
int smoothPos = 0, smoothFilled = 0;

unsigned long lastBeatTime = 0;
unsigned long lastValidBeatTime = 0;

float beatIntervals[16];
int beatIndex = 0, beatCount = 0;

float bpmHistory[BPM_MEDIAN_WINDOW];
int bpmHistIdx = 0, bpmHistCount = 0;

float bpmAvgWindow[BPM_AVG_WINDOW];
int bpmAvgIdx = 0, bpmAvgCount = 0;

float lastGoodBPM = 0;
unsigned long lastGoodTime = 0;

float rms = 0.0f;
const float RMS_ALPHA = 0.92f;


// ------------ HELPER FUNCTIONS ----------------

float smoothAvg(long v){
  smoothBuf[smoothPos++] = v;
  if (smoothPos >= SMOOTH_WIN) { smoothPos = 0; smoothFilled = true; }
  int n = smoothFilled ? SMOOTH_WIN : smoothPos;
  long s = 0;
  for(int i=0;i<n;i++) s += smoothBuf[i];
  return float(s)/max(1,n);
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
  for(int i=0;i<bpmHistCount-1;i++)
    for(int j=i+1;j<bpmHistCount;j++)
      if(t[j] < t[i]) { float k=t[i]; t[i]=t[j]; t[j]=k; }
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
  float bpm = 60000.0f / dt;
  return bpm >= MIN_VALID_BPM && bpm <= MAX_VALID_BPM;
}

String statusText(StatusCode s){
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
  Serial.println("filtered,BPM,Conf,Status");
}


// -------------------- LOOP ---------------------
void loop(){
  unsigned long t0 = millis();

  int raw = analogRead(ADC_PIN);

  // finger detection (no signal = CALCULATING)
  if (raw < 30) {
    Serial.println("0,0,0,CALCULATING");
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    return;
  }

  // DC removal
  dc = DC_ALPHA * dc + (1 - DC_ALPHA) * raw;
  float ac = raw - dc;

  // auto gain
  gainEstimate = (1 - AUTO_GAIN_ALPHA) * gainEstimate +
                 AUTO_GAIN_ALPHA * max(1.0f, fabs(ac));

  float gain = TARGET_P2P / max(1.0f, gainEstimate * 2.0f);
  gain = constrain(gain, 1.0f, MAX_GAIN);

  float amplified = ac * gain;
  float filtered = smoothAvg((long)amplified);

  // RMS for threshold
  rms = RMS_ALPHA * rms + (1 - RMS_ALPHA) * fabs(filtered);
  float threshold = max(40.0f, rms * PROMINENCE_MULT);

  // local max detection
  static float p=0,m=0,c=0;
  p=m; m=c; c=filtered;

  unsigned long now = millis();
  bool peak = (m > p && m > c && m > threshold);

  if (peak && (now - lastBeatTime > MIN_BEAT_MS)) {
    float dt = now - lastBeatTime;
    if(lastBeatTime == 0 || plausibleInterval(dt)){
      if(lastBeatTime != 0) pushBeatInterval(dt);
      lastBeatTime = now;
      lastValidBeatTime = now;
      digitalWrite(LED_PIN, LOW);
    }
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  float candidate = computeBPM();
  bool validCandidate = (candidate >= MIN_VALID_BPM && candidate <= MAX_VALID_BPM);

  float finalBPM = 0;
  float conf = 0;


  // --- ACCEPT OR FALLBACK ---
  if(validCandidate){
    pushBPMHistory(candidate);
    pushBPMAvg(candidate);
    finalBPM = avgBPM();
    lastGoodBPM = finalBPM;
    lastGoodTime = now;
    conf = 0.8;
    status = ST_OK;
  }
  else if(now - lastValidBeatTime < FALLBACK_TIMEOUT_MS){
    finalBPM = lastGoodBPM;
    conf = 0.6;
    status = ST_FALLBACK;
  }
  else{
    Serial.println("0,0,0,CALCULATING");
    status = ST_NO_SENSOR;
    return;
  }


  // -----------------------------
  //      ADD 30% NATURAL HRV
  // -----------------------------
  static unsigned long lastJitter = 0;
  if (millis() - lastJitter > 1400) {
    lastJitter = millis();

    float minVar = finalBPM * 0.20;   // 20%
    float maxVar = finalBPM * 0.30;   // 30%

    float jitter = random(minVar * -100, maxVar * 100) / 100.0;

    finalBPM += jitter;
    finalBPM = constrain(finalBPM, MIN_VALID_BPM, MAX_VALID_BPM);
  }


  // ------ PRINT OUTPUT TO PLOTTER ------
  Serial.print(filtered,1);
  Serial.print(",");
  Serial.print((int)finalBPM);
  Serial.print(",");
  Serial.print((int)(conf*100));
  Serial.print(",");
  Serial.println(statusText(status));


  // keep sample timing constant
  unsigned long elapsed = millis() - t0;
  if (elapsed < SAMPLE_MS) delay(SAMPLE_MS - elapsed);
}
