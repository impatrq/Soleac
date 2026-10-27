# SOLEAC — aplicación móvil

Aplicación desarrollada con Flutter para consultar desde un teléfono la telemetría del detector SOLEAC mediante Bluetooth Low Energy (BLE).

## Qué contiene

- `lib/main.dart`: interfaz y lógica de la aplicación. Incluye las secciones **Panel**, **Historial**, **Ajustes** y **Equipo**, además de temas claro y oscuro.
- `lib/soleac_ble_service.dart`: búsqueda del dispositivo SOLEAC, conexión BLE, lectura de telemetría y envío de comandos para el modo demo.
- `android/` y `web/`: configuración de las plataformas incluidas actualmente en el proyecto.
- `android_ble_permissions_snippet.xml`: permisos BLE de referencia para Android.
- `ios_info_plist_snippet.xml`: claves de privacidad Bluetooth de referencia para iOS.

## Requisitos

- Flutter con Dart SDK `>=3.4.0 <4.0.0`.
- Un dispositivo Android con Bluetooth LE para conectarse al detector.
- El detector SOLEAC encendido y dentro del alcance Bluetooth.

Las dependencias de Flutter están declaradas en `pubspec.yaml`. La app usa `flutter_reactive_ble` para BLE, `permission_handler` para permisos y `shared_preferences` para guardar preferencias locales.

## Ejecutar en Android

Desde esta carpeta:

```sh
flutter pub get
flutter run
```

Si se crea o reconstruye el proyecto nativo Android, revisar los permisos requeridos en `android_ble_permissions_snippet.xml` y agregarlos a `android/app/src/main/AndroidManifest.xml` según la versión de Android.

## Conexión BLE

La app busca un dispositivo con el nombre `SOLEAC` y usa estos identificadores:

| Elemento | UUID |
| --- | --- |
| Servicio | `12345678-1234-5678-1234-56789abcdef0` |
| Característica de telemetría | `12345678-1234-5678-1234-56789abcdef1` |
| Característica de comandos | `12345678-1234-5678-1234-56789abcdef2` |

El servicio interpreta telemetría recibida como JSON, lista compacta o CSV. El formato concreto debe coincidir con el firmware instalado en el detector.

## iOS

El proyecto incluye el fragmento de privacidad Bluetooth en `ios_info_plist_snippet.xml` como referencia. Para ejecutar en iOS se necesita generar/configurar la plataforma iOS de Flutter y añadir las claves al `Info.plist` de Runner.

## Estado

Versión inicial `0.1.0+1`. Algunas opciones de la interfaz, como las notificaciones push, pueden mostrarse como ajustes pero aún no tienen una integración funcional.
