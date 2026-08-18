# Test_Arduino

Dentro de esta carpeta se encuetran pruebas realizadas en Arduino IDE.
Estas pruebas estan hechas para que la applicacion movil de Flutter detecte las frecuencias y las muestre en forma de dB (Ganancia). 
A continuacion se describe lo que contiene cada archivo:
#
## 1_envio_de_datos 
 Configura la ESP32 como dispositivo BLE llamado SOLEAC y envía datos simulados a la aplicación Flutter. La intensidad hf sube y baja entre 25 y 75, generando una alarma cuando supera el umbral de 60.

## 2_recepcion_de_frecuencia:
 Prueba básica del micrófono MAX4466. Lee directamente el valor analógico del pin 34 y lo muestra por el monitor serial.

## 3_recepcion_de_frecuencia:
Primera versión funcional con micrófono y BLE.
Toma 300 muestras del MAX4466.

-Calcula el valor pico a pico de la señal.

-Convierte ese valor a una escala aproximada de intensidad hf entre 0 y 100.

-Compara la intensidad con el umbral de 60.

-Envía los datos a Flutter mediante BLE en formato CSV:

## 4_recepcion_de_frecuencia:
 Hace lo mismo que 3_recepcion_de_frecuencia, pero:

-Muestra también peakToPeak por el monitor serial.

-Usa un rango de conversión diferente, de 0 a 250.

-Reinicia el advertising BLE continuamente cuando no hay conexión.

-Envía el mismo formato CSV compatible con Flutter.

## 5_lcd_aparte:Es una prueba independiente de la pantalla LCD del dispositivo ESP32-S3.

-Inicializa la pantalla mediante QSPI.

-Configura el expansor I²C TCA9554.

-Activa la retroiluminación.

-Comprueba la PSRAM.

-Muestra los textos SOLEAC, LCD OK, ESP32-S3 y PSRAM OK.

## libraries: 
Librerias necesarios para los  upload.
# Como probar estos codigos
Para subir el codigo a la Esp32, se debe conectar mediante un usb en algun puerto que posea su PC y haga click en upload.

Para mostrar lo que recibe la ESP32, debera abrir el monitor en Arduino IDE en 115200 bau
