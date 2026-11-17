constexpr uint8_t HC_SR04_TRIG_PIN = 25;

void setup()
{
    pinMode(HC_SR04_TRIG_PIN, OUTPUT);
    digitalWrite(HC_SR04_TRIG_PIN, LOW);
    delay(5000);
}

void loop()
{
    digitalWrite(HC_SR04_TRIG_PIN, LOW);
    delayMicroseconds(3);
    digitalWrite(HC_SR04_TRIG_PIN, HIGH);
    delayMicroseconds(12);
    digitalWrite(HC_SR04_TRIG_PIN, LOW);
    delay(100);
}
