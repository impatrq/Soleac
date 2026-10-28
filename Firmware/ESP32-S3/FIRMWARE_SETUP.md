# Configuración de compilación del firmware

## Entorno utilizado

- ESP-IDF: 6.0.2 (según el `sdkconfig` de este proyecto y `dependencies.lock`).
- Target: ESP32-S3.
- Flash configurada: 16 MB.
- PSRAM: habilitada en modo octal, 80 MHz, asignación mediante `malloc`.
- Bluetooth: BLE con NimBLE.
- Componentes administrados: declarados en `main/idf_component.yml` y fijados en `dependencies.lock`.

La carpeta `.espressif/` contiene herramientas instaladas localmente y metadatos de la computadora. No se necesita subirla al repositorio: se instala ESP-IDF y sus herramientas en cada entorno de desarrollo.

## Compilar

Con ESP-IDF 6.0.2 instalado y su entorno activado, desde la raíz del proyecto:

```bash
idf.py set-target esp32s3
idf.py build
```

ESP-IDF obtiene las dependencias descritas por el manifiesto y el archivo de bloqueo. `build/` y `managed_components/` son resultados locales y están excluidos de Git.

## Archivos de configuración versionados

- `sdkconfig.defaults`: valores predeterminados del proyecto para crear una configuración nueva.
- `sdkconfig`: configuración completa generada y usada en este entorno; se conserva para documentar la configuración actual de la placa.
- `dependencies.lock`: versiones concretas de ESP-IDF Component Manager y sus componentes.

Hay una diferencia que debe revisarse: `sdkconfig.defaults` deshabilita Wi-Fi, pero `sdkconfig` lo tiene habilitado. Hasta confirmar cuál es el comportamiento intencionado, se conservan ambos archivos sin alterar sus valores.
