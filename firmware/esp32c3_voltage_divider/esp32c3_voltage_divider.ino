/*
  ESP32-C3-DevKit Voltage Reader (27k / 22k divider)

  ---------------- ARDUINO IDE SETTINGS ----------------
  Tools -> Board            : ESP32C3 Dev Module
  Tools -> USB CDC On Boot  :
       * Disabled  -> if you plug into the official DevKit's micro-USB
                      port (it has a USB-to-UART chip, e.g. CP2102)
       * Enabled   -> if your board uses the C3's native USB
                      (most USB-C clones / SuperMini boards)
  Tools -> Upload Speed     : 115200 or 921600
  Serial Monitor baud       : 115200
  ------------------------------------------------------

  Wiring:
    Vin ---[ R1 = 27k ]---+---[ R2 = 22k ]--- GND
                          |
                        GPIO3
    Connect the measured source's GND to the ESP32-C3 GND.

  Vin = Vout * (R1 + R2) / R2 = Vout * 2.227
  Max safe Vin ~= 5.5 V (C3 ADC is accurate up to ~2.5 V)
*/

const int   ADC_PIN     = 3;        // GPIO3 = ADC1_CH3
const int   LED_PIN     = 8;        // On-board LED on most C3 DevKits (blinks = code is running)
const float R1          = 27000.0;
const float R2          = 22000.0;
const int   NUM_SAMPLES = 64;
const float CALIBRATION = 1.012;     // Adjust after comparing with a multimeter

void setup() {
  Serial.begin(115200);

  // Wait up to 3 s for the Serial Monitor (needed for native USB)
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {
    delay(10);
  }

  pinMode(LED_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN, ADC_11db);

  Serial.println();
  Serial.println("=== ESP32-C3 Voltage Reader started ===");
}

float readPinVoltage() {
  uint32_t total = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    total += analogReadMilliVolts(ADC_PIN);
    delayMicroseconds(200);
  }
  return (total / (float)NUM_SAMPLES) / 1000.0;
}

void loop() {
  int   raw  = analogRead(ADC_PIN);                     // 0 - 4095, for debugging
  float vOut = readPinVoltage();                        // Volts at GPIO3
  float vIn  = vOut * ((R1 + R2) / R2) * CALIBRATION;   // Actual input voltage

  Serial.print("Raw: ");
  Serial.print(raw);
  Serial.print("  |  Pin: ");
  Serial.print(vOut, 3);
  Serial.print(" V  |  Input: ");
  Serial.print(vIn, 2);
  Serial.println(" V");

  digitalWrite(LED_PIN, !digitalRead(LED_PIN));  // Heartbeat blink
  delay(1000);
}
