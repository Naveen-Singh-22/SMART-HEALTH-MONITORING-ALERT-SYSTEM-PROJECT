// Simple no-library pulse detector for Arduino UNO
// Signal -> A0, VCC -> 5V, GND -> GND
// LED on pin 13 blinks on beat
const int SIG_PIN = A0;
const int LED_PIN = 13;

const float alpha = 0.95;      // DC removal factor (0.90..0.99)
const int LP_SIZE = 5;         // smoothing window
const unsigned long MIN_BEAT_MS = 300;
const int BEAT_BUF = 8;

float dc = 0.0;
long lpBuf[LP_SIZE];
int lpPos = 0;
bool lpFilled = false;

unsigned long lastBeat = 0;
float beatIntervals[BEAT_BUF];
int beatIndex = 0;
int beatsStored = 0;

float movingAverage(long v) {
  lpBuf[lpPos++] = v;
  if (lpPos >= LP_SIZE) { lpPos = 0; lpFilled = true; }
  int n = lpFilled ? LP_SIZE : lpPos;
  long s = 0;
  for (int i=0;i<n;i++) s += lpBuf[i];
  return float(s) / max(1, n);
}

float computeBPM() {
  if (beatsStored == 0) return 0.0;
  float s = 0;
  for (int i=0;i<beatsStored;i++) s += beatIntervals[i];
  float avg = s / beatsStored;
  if (avg <= 0) return 0;
  return 60000.0 / avg;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  for (int i=0;i<LP_SIZE;i++) lpBuf[i]=0;
  dc = 0.0;
  Serial.println("Simple Pulse Detector starting...");
}

void loop() {
  int raw = analogRead(SIG_PIN); // 0..1023
  // DC removal
  dc = alpha * dc + (1.0 - alpha) * raw;
  float ac = raw - dc;

  // smoothing
  float filtered = movingAverage((long)(ac * 100)); // scale to make peaks visible

  // adaptive threshold based on running RMS-ish (approx using abs)
  static float rms = 0;
  const float rmsAlpha = 0.95;
  rms = rmsAlpha * rms + (1 - rmsAlpha) * fabs(filtered);

  float threshold = max( (float)80.0, rms * 1.8 ); // tune multiplier 1.2..3.0

  unsigned long now = millis();
  static bool wasAbove = false;

  bool isAbove = (filtered > threshold);

  if (isAbove && !wasAbove) {
    // rising edge
    if (lastBeat == 0 || (now - lastBeat) > MIN_BEAT_MS) {
      if (lastBeat != 0) {
        unsigned long dt = now - lastBeat;
        beatIntervals[beatIndex] = (float)dt;
        beatIndex = (beatIndex + 1) % BEAT_BUF;
        if (beatsStored < BEAT_BUF) beatsStored++;
      }
      lastBeat = now;
    }
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }
  wasAbove = isAbove;

  float bpm = computeBPM();

  // Print filtered signal and bpm for debugging
  Serial.print(filtered, 1);
  Serial.print(',');
  Serial.println((int)(bpm+0.5));

  delay(20);
}
