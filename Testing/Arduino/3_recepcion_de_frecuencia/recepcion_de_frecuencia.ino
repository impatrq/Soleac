const int MIC_PIN = 34;

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
}

void loop() {
  int value = analogRead(MIC_PIN);
  Serial.println(value);
  delay(50);
}