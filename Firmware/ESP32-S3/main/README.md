# Firmware SOLEAC

Esta carpeta contiene el componente principal del firmware para el detector SOLEAC basado en ESP32-S3. Aquí están el punto de entrada de la aplicación, los módulos de procesamiento y periféricos, y la configuración de compilación del componente ESP-IDF.

## Archivos

- `app_main.c`: inicialización y coordinación de las tareas y módulos del firmware.
- `ble_comm.c` / `ble_comm.h`: comunicación BLE, recepción de comandos y envío de telemetría.
- `fft_processing.c` / `fft_processing.h`: captura y procesamiento de señales, cálculo FFT, estado del detector y manejo de alarmas/modo de prueba.
- `lcd_control.c` / `lcd_control.h`: inicialización, diagnóstico, pantalla y lectura de interacción táctil.
- `sd_logger.c` / `sd_logger.h`: registro en tarjeta SD del estado y las mediciones del detector.
- `CMakeLists.txt`: registro del componente y declaración de los componentes ESP-IDF requeridos para compilarlo.
- `idf_component.yml`: dependencias administradas del componente.

La configuración general del proyecto ESP-IDF está en la raíz del repositorio. Las dependencias administradas se describen en `idf_component.yml` y se fijan en el archivo `dependencies.lock` de la raíz.
