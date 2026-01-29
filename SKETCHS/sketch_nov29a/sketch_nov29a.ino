// --- USER CONFIGURATION ---
// Define the GPIO pin connected to the buzzer.
// D7 is mapped to GPIO13 on the NodeMCU ESP8266 development board.
#define BUZZER_PIN D7 // Use D7, which corresponds to GPIO13

// Define the duration the buzzer will be ON (in milliseconds)
#define BUZZ_DURATION 200 

// --- SETUP ---
void setup() {
  // Initialize the buzzer pin as an OUTPUT
  pinMode(BUZZER_PIN, OUTPUT);

  // You might want to include serial for debugging
  Serial.begin(115200);
  Serial.println("Buzzer Test Ready.");
}

// --- MAIN LOOP ---
void loop() {
  // Call the function to trigger a single short buzz
  triggerBuzzerAlert();

  // Wait for 5 seconds before the next test buzz
  delay(5000); 
}

// --- CUSTOM FUNCTION ---
// Function to activate the buzzer for a short period
void triggerBuzzerAlert() {
  Serial.println("ALERT: Activating Buzzer...");
  
  // 1. Activate the buzzer by setting the pin HIGH
  digitalWrite(BUZZER_PIN, HIGH); 
  
  // 2. Keep it on for the defined duration
  delay(BUZZ_DURATION); 
  
  // 3. Deactivate the buzzer by setting the pin LOW
  digitalWrite(BUZZER_PIN, LOW); 
  
  Serial.println("Buzzer Alert Complete.");
}

// --- OPTIONAL: Integrating with Logic (like a health alert) ---
/* // Example of how you would integrate this into a functional alert loop:

void checkVitalsAndAlert(float currentTemp) {
  // Assuming a temperature threshold (like the project uses > 38.0C) [cite: 1090]
  if (currentTemp > 38.0) { 
    triggerBuzzerAlert();
    // Also send cloud notification here (e.g., Blynk.notify("High Temp!")) [cite: 160, 446]
  }
}
*/