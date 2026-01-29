int buzzer = D5; // signal pin

void setup() {
  pinMode(buzzer, OUTPUT);
}

void loop() {
  digitalWrite(buzzer, HIGH);  // buzzer ON
  delay(500);                  // 0.5 sec
  digitalWrite(buzzer, LOW);   // buzzer OFF
  delay(500);
}
