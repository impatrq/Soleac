# SOLEAC Flutter BLE

Esta carpeta deja listo el lado Flutter para conectarse al BLE del firmware actual.

Datos detectados en `main/detector_fuga_main.c`:

- Nombre BLE: `SOLEAC`
- Servicio: `12345678-1234-5678-1234-56789abcdef0`
- Caracteristica de telemetria: `12345678-1234-5678-1234-56789abcdef1`
- Payload normal: `{"audio":0.0,"hf":0.0,"threshold":250.0,"alarm":false,"demo":false}`
- Payload compacto si el MTU es chico: `[audio,hf,threshold,alarm,demo]`

## Usarlo como app nueva

Desde esta carpeta:

```powershell
flutter create --platforms=android,ios .
flutter pub get
flutter run
```

Despues agrega los permisos:

- Android: usar `android_ble_permissions_snippet.xml`.
- iOS: usar `ios_info_plist_snippet.xml`.

## Usarlo dentro de tu app ya hecha

Agrega estas dependencias a tu `pubspec.yaml`:

```yaml
dependencies:
  flutter_reactive_ble: 5.5.0
  permission_handler: 12.0.1
```

Importa el servicio:

```dart
import 'soleac_ble_service.dart';
```

Ejemplo minimo:

```dart
final ble = SoleacBleService();

await ble.connect();

ble.telemetry.listen((data) {
  print(data.audio);
  print(data.highFrequency);
  print(data.threshold);
  print(data.alarm);
});
```
