#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

// --- Paste the following into your .ino (after includes / before end of file) ---
// Maxim heart rate & SpO2 algorithm implementation
void maxim_heart_rate_and_oxygen_saturation(
  uint32_t *irBuffer, int irBufferLength,
  uint32_t *redBuffer, int redBufferLength,
  int32_t *spo2, int8_t *spo2Valid,
  int32_t *heartRate, int8_t *heartRateValid
){
  int i;
  uint32_t irMean, redMean;
  float sumX, sumY, sumXY, sumX2;
  int32_t heartRateTemp = 0;
  int32_t spo2Temp = 0;
  int8_t hrValidTemp = 0;
  int8_t spo2ValidTemp = 0;

  // Compute means
  irMean = 0;
  redMean = 0;
  for (i = 0; i < irBufferLength; i++) {
    irMean += irBuffer[i];
    redMean += redBuffer[i];
  }
  irMean /= irBufferLength;
  redMean /= redBufferLength;

  // Remove DC
  for (i = 0; i < irBufferLength; i++) {
    irBuffer[i] -= irMean;
    redBuffer[i] -= redMean;
  }

  // Compute AC/DC Ratio using least squares method (simple linear fit approach)
  sumX = sumY = sumXY = sumX2 = 0.0f;
  for (i = 0; i < irBufferLength; i++) {
    sumX += (float)irBuffer[i];
    sumY += (float)redBuffer[i];
    sumXY += (float)irBuffer[i] * (float)redBuffer[i];
    sumX2 += (float)irBuffer[i] * (float)irBuffer[i];
  }

  float denom = (sumX * sumX2 - sumX * sumX);
  float ratio = 0.0f;
  if (fabs(denom) > 1e-6) {
    ratio = (sumY * sumX2 - sumX * sumXY) / denom;
  } else {
    ratio = 0.0f;
  }

  // Empirical conversion to SpO2 (calibration equation used by many examples)
  spo2Temp = (int32_t)(104.0f - 17.0f * ratio);

  // Heart rate detection via zero-crossing (very simple)
  int peaks = 0;
  for (i = 1; i < irBufferLength; i++) {
    if (irBuffer[i] > 0 && irBuffer[i - 1] <= 0) peaks++;
  }

  // If buffer ~1 second at 100 Hz, peaks * 60 ~= BPM
  heartRateTemp = peaks * 60;

  if (heartRateTemp > 30 && heartRateTemp < 200) hrValidTemp = 1;
  if (spo2Temp > 50 && spo2Temp < 100) spo2ValidTemp = 1;

  *spo2 = spo2Temp;
  *heartRate = heartRateTemp;
  *spo2Valid = spo2ValidTemp;
  *heartRateValid = hrValidTemp;
}


// Maxim algorithm forward declaration
void maxim_heart_rate_and_oxygen_saturation(
  uint32_t *irBuffer, int irBufferLength, 
  uint32_t *redBuffer, int redBufferLength, 
  int32_t *spo2, int8_t *spo2Valid, 
  int32_t *heartRate, int8_t *heartRateValid
);

MAX30105 particleSensor;

#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(D2, D1); // SDA = D2 (GPIO4), SCL = D1 (GPIO5)

  Serial.println("Initializing MAX30102...");

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("Sensor NOT found. Check wiring.");
    while (1);
  }

  // Recommended settings for SPO2
  particleSensor.setup(0x1F, 4, 2, 100, 411); 
  particleSensor.setPulseAmplitudeIR(0x7F);
  particleSensor.setPulseAmplitudeRed(0x7F);

  Serial.println("MAX30102 initialized.");
}

void loop() {
  Serial.println("---- Measurement ----");

  // Fill buffers
  for (int i = 0; i < BUFFER_SIZE; i++) {
    while (!particleSensor.available())
      particleSensor.check();

    redBuffer[i] = particleSensor.getRed();
    irBuffer[i]  = particleSensor.getIR();
    particleSensor.nextSample();
    delay(10);
  }

  int32_t spo2, heartRate;
  int8_t spo2Valid, heartRateValid;

  // Run Maxim algorithm
  maxim_heart_rate_and_oxygen_saturation(
    irBuffer, BUFFER_SIZE, 
    redBuffer, BUFFER_SIZE, 
    &spo2, &spo2Valid, 
    &heartRate, &heartRateValid
  );

  // Print result
  if (spo2Valid)
    Serial.print("SpO2: "), Serial.print(spo2), Serial.println(" %");
  else
    Serial.println("SpO2: invalid");

  if (heartRateValid)
    Serial.print("Heart Rate: "), Serial.print(heartRate), Serial.println(" BPM");
  else
    Serial.println("Heart Rate: invalid");

  Serial.println("----------------------");
  delay(100);
}
