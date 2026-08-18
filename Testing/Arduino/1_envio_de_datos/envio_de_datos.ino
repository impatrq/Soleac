#include <NimBLEDevice.h>

#define DEVICE_NAME "SOLEAC"

#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define TELEMETRY_CHAR_UUID "12345678-1234-5678-1234-56789abcdef1"

NimBLECharacteristic* telemetryCharacteristic;

float hf = 35.0;
float stepValue = 2.5;
const float threshold = 60.0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Iniciando SOLEAC BLE demo...");

  NimBLEDevice::init(DEVICE_NAME);

  NimBLEServer* server = NimBLEDevice::createServer();
  NimBLEService* service = server->createService(SERVICE_UUID);

  telemetryCharacteristic = service->createCharacteristic(
    TELEMETRY_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setName(DEVICE_NAME);
  advertising->start();

  Serial.println("BLE listo. Dispositivo anunciado como SOLEAC.");
}

void loop() {
  hf += stepValue;

  if (hf >= 75.0 || hf <= 25.0) {
    stepValue = -stepValue;
  }

  bool alarm = hf >= threshold;

  String payload = "{";
  payload += "\"audio\":0.0,";
  payload += "\"hf\":" + String(hf, 1) + ",";
  payload += "\"threshold\":" + String(threshold, 1) + ",";
  payload += "\"alarm\":" + String(alarm ? "true" : "false") + ",";
  payload += "\"demo\":true";
  payload += "}";

  telemetryCharacteristic->setValue(payload.c_str());
  telemetryCharacteristic->notify();

  Serial.println(payload);

  delay(1000);
}