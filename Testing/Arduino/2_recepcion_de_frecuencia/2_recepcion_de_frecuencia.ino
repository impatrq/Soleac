#include <NimBLEDevice.h>
#include <math.h>

#define DEVICE_NAME "SOLEAC"

#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define TELEMETRY_CHAR_UUID "12345678-1234-5678-1234-56789abcdef1"

const int MIC_PIN = 34;

const float threshold = 60.0;

NimBLECharacteristic* telemetryCharacteristic;
bool deviceConnected = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server) {
    deviceConnected = true;
    Serial.println("Celular conectado por BLE");
  }

  void onDisconnect(NimBLEServer* server) {
    deviceConnected = false;
    Serial.println("Celular desconectado. Reiniciando advertising...");
    delay(500);
    NimBLEDevice::startAdvertising();
  }
};

float readMicLevel() {
  const int samples = 300;

  int minValue = 4095;
  int maxValue = 0;

  for (int i = 0; i < samples; i++) {
    int value = analogRead(MIC_PIN);

    if (value < minValue) {
      minValue = value;
    }

    if (value > maxValue) {
      maxValue = value;
    }

    delayMicroseconds(150);
  }

  int peakToPeak = maxValue - minValue;

  float hf = map(peakToPeak, 0, 600, 20, 95);
  hf = constrain(hf, 0, 100);

  return hf;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  Serial.println("Iniciando SOLEAC BLE + microfono...");

  NimBLEDevice::init(DEVICE_NAME);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);

  telemetryCharacteristic = service->createCharacteristic(
    TELEMETRY_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  telemetryCharacteristic->setValue("20.0,60.0,0,1");

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setName(DEVICE_NAME);
  advertising->start();

  Serial.println("BLE listo. Buscar SOLEAC desde la app.");
}

void loop() {
  float hf = readMicLevel();
  bool alarm = hf >= threshold;

  String payload = "";
  payload += String(hf, 1);
  payload += ",";
  payload += String(threshold, 1);
  payload += ",";
  payload += alarm ? "1" : "0";
  payload += ",";
  payload += "1";

  telemetryCharacteristic->setValue(payload.c_str());

  if (deviceConnected) {
    telemetryCharacteristic->notify();
  }

  Serial.print("payload: ");
  Serial.println(payload);

  delay(500);
}