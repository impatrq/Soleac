#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"

// Pines de la pantalla QSPI
#define LCD_QSPI_CS   12
#define LCD_QSPI_CLK   5
#define LCD_QSPI_D0    1
#define LCD_QSPI_D1    2
#define LCD_QSPI_D2    3
#define LCD_QSPI_D3    4

// Retroiluminación
#define LCD_BACKLIGHT 6

// I2C del expansor TCA9554
#define I2C_SDA 21
#define I2C_SCL 22

TCA9554 tca(0x20);

// Bus QSPI de la pantalla
Arduino_DataBus* bus = new Arduino_ESP32QSPI(
  LCD_QSPI_CS,
  LCD_QSPI_CLK,
  LCD_QSPI_D0,
  LCD_QSPI_D1,
  LCD_QSPI_D2,
  LCD_QSPI_D3
);

// Controlador AXS15231B de la pantalla
Arduino_GFX* display = new Arduino_AXS15231B(
  bus,
  GFX_NOT_DEFINED,
  0,
  false,
  320,
  480
);

// Canvas usando la PSRAM
Arduino_Canvas* gfx = new Arduino_Canvas(
  320,
  480,
  display,
  0,
  0,
  0
);

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("================================");
  Serial.println("Prueba LCD ESP32-S3 SOLEAC");
  Serial.println("================================");

  Serial.print("PSRAM encontrada: ");
  Serial.println(psramFound() ? "SI" : "NO");

  Serial.print("PSRAM total: ");
  Serial.print(ESP.getPsramSize() / 1024);
  Serial.println(" KB");

  Serial.print("Heap libre: ");
  Serial.print(ESP.getFreeHeap() / 1024);
  Serial.println(" KB");

  Serial.println("Inicializando I2C...");

  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println("Inicializando TCA9554...");

  tca.begin();

  // Control de alimentación/backlight mediante el expansor
  tca.pinMode1(1, OUTPUT);

  tca.write1(1, 1);
  delay(10);

  tca.write1(1, 0);
  delay(10);

  tca.write1(1, 1);
  delay(200);

  Serial.println("Activando retroiluminacion...");

  pinMode(LCD_BACKLIGHT, OUTPUT);
  digitalWrite(LCD_BACKLIGHT, HIGH);

  Serial.println("Inicializando controlador LCD...");

  if (!gfx->begin()) {
    Serial.println("ERROR: no se pudo inicializar el canvas/LCD");
    return;
  }

  Serial.println("LCD inicializado correctamente");

  gfx->fillScreen(RGB565_BLUE);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4);
  gfx->setCursor(30, 60);
  gfx->println("SOLEAC");

  gfx->setTextSize(3);
  gfx->setCursor(30, 140);
  gfx->println("LCD OK");

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(30, 210);
  gfx->println("ESP32-S3");

  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(30, 280);
  gfx->println("PSRAM OK");

  // Envía el contenido del canvas a la pantalla
  gfx->flush();

  Serial.println("Texto enviado al LCD");
  Serial.println("Prueba terminada");
}

void loop() {
  delay(1000);
}