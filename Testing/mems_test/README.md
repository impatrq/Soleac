# Prueba aislada del MEMS SPH0641

Este proyecto es independiente del detector SOLEAC. Captura PDM en GPIO42/41 y
reporta cada 0.5 s el nivel RMS total y la energía estimada en 1 kHz, 12 kHz y 40 kHz.
También calcula `pico_40k` en ventanas de 1 ms para captar las ráfagas cortas del HC-SR04.
SELECT debe permanecer conectado a GND (slot LEFT).

## Flashear y abrir el monitor

Desde la raíz del repositorio, con ESP-IDF 6 activado:

```bash
idf.py -C mems_test -p /dev/ttyACM0 flash monitor
```

Si el puerto cambió, consultarlo primero:

```bash
python3 -m serial.tools.list_ports -v
```

## Medición

1. Dejar el MEMS en silencio al inicio y observar varios informes.
2. Reproducir un tono continuo de 1 kHz cerca del orificio acústico.
3. Comparar `tono_1k` con el silencio; repetir con un tono continuo de 12 kHz y
   comparar `tono_12k`. Cada lectura debería subir con su tono correspondiente.
4. `captura=OK` confirma que se completó el bloque; `unos` entre aproximadamente 5 % y 95 % indica actividad en DATA.
   `errores=0` indica que las lecturas I2S/PDM se completan sin timeout.

## Prueba con HC-SR04 y Arduino Nano

- HC-SR04 `VCC` a `5V` del Nano, `GND` a `GND` del Nano y `TRIG` a `D9`.
- Dejar `ECHO` sin conectar: esta prueba solo usa el emisor del módulo.
- Unir `GND` del Nano con `GND` de la ESP32 para tener referencia común.
- Apuntar el emisor del HC-SR04 al puerto del MEMS, a unos pocos centímetros.
- Cargar `hc_sr04_trigger/hc_sr04_trigger.ino` en el Nano. Espera 5 s y luego emite una ráfaga cada 100 ms.
- Comparar `pico_40k` con el valor en silencio; el promedio `tono_40k` puede ser bajo
  porque las ráfagas duran solo unos 200 us.

Las magnitudes `RMS`, `tono_1k`, `tono_12k` y `tono_40k` son unidades digitales relativas,
no dB SPL ni voltios. Para volver al detector normal, flashear desde la raíz:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```
