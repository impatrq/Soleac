import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:permission_handler/permission_handler.dart';

const String soleacDeviceName = 'SOLEAC';
final Uuid soleacServiceUuid =
    Uuid.parse('12345678-1234-5678-1234-56789abcdef0');
final Uuid soleacTelemetryCharacteristicUuid =
    Uuid.parse('12345678-1234-5678-1234-56789abcdef1');
final Uuid soleacCommandCharacteristicUuid =
    Uuid.parse('12345678-1234-5678-1234-56789abcdef2');

enum SoleacBlePhase {
  idle,
  checkingAdapter,
  scanning,
  connecting,
  connected,
  disconnected,
  error,
}

class SoleacBleStatus {
  const SoleacBleStatus(this.phase, this.message);

  final SoleacBlePhase phase;
  final String message;

  bool get isConnected => phase == SoleacBlePhase.connected;
}

class SoleacTelemetry {
  const SoleacTelemetry({
    required this.audio,
    required this.highFrequency,
    required this.threshold,
    required this.alarm,
    required this.demo,
    required this.raw,
    required this.receivedAt,
  });

  final double audio;
  final double highFrequency;
  final double threshold;
  final bool alarm;
  final bool demo;
  final String raw;
  final DateTime receivedAt;

  factory SoleacTelemetry.fromBleBytes(List<int> bytes) {
    final raw = utf8.decode(bytes, allowMalformed: true).trim();

    if (_looksLikeCsv(raw)) {
      return _fromCsv(raw);
    }

    final decoded = jsonDecode(raw);

    if (decoded is Map<String, dynamic>) {
      return _fromMap(decoded, raw);
    }

    if (decoded is List && decoded.length >= 5) {
      return SoleacTelemetry(
        audio: _asDouble(decoded[0]),
        highFrequency: _asDouble(decoded[1]),
        threshold: _asDouble(decoded[2]),
        alarm: _asBool(decoded[3]),
        demo: _asBool(decoded[4]),
        raw: raw,
        receivedAt: DateTime.now(),
      );
    }

    throw const FormatException('Formato de telemetría SOLEAC no reconocido');
  }

  static bool looksIncompletePayload(List<int> bytes) {
    final raw = utf8.decode(bytes, allowMalformed: true).trim();
    if (raw.isEmpty) {
      return true;
    }
    return (raw.startsWith('{') && !raw.endsWith('}')) ||
        (raw.startsWith('[') && !raw.endsWith(']'));
  }

  static SoleacTelemetry _fromMap(Map<String, dynamic> decoded, String raw) {
    return SoleacTelemetry(
      audio: _asDouble(decoded['audio'] ?? decoded['au']),
      highFrequency: _asDouble(decoded['hf'] ?? decoded['h']),
      threshold: _asDouble(decoded['threshold'] ?? decoded['t']),
      alarm: _asBool(decoded['alarm'] ?? decoded['a']),
      demo: _asBool(decoded['demo'] ?? decoded['d']),
      raw: raw,
      receivedAt: DateTime.now(),
    );
  }

  static SoleacTelemetry _fromCsv(String raw) {
    final values = raw.split(',').map((value) => value.trim()).toList();
    if (values.length < 4) {
      throw const FormatException('CSV de telemetría SOLEAC incompleto');
    }
    return SoleacTelemetry(
      audio: 0,
      highFrequency: _asDouble(values[0]),
      threshold: _asDouble(values[1]),
      alarm: _asBool(values[2]),
      demo: _asBool(values[3]),
      raw: raw,
      receivedAt: DateTime.now(),
    );
  }

  static bool _looksLikeCsv(String raw) {
    return !raw.startsWith('{') && !raw.startsWith('[') && raw.contains(',');
  }

  static double _asDouble(Object? value) {
    if (value is num) {
      return value.toDouble();
    }
    if (value is String) {
      return double.tryParse(value) ?? 0;
    }
    return 0;
  }

  static bool _asBool(Object? value) {
    if (value is bool) {
      return value;
    }
    if (value is num) {
      return value != 0;
    }
    if (value is String) {
      final normalized = value.trim().toLowerCase();
      return normalized == 'true' || normalized == '1' || normalized == 'yes';
    }
    return false;
  }
}

class SoleacBleException implements Exception {
  const SoleacBleException(this.message);

  final String message;

  @override
  String toString() => message;
}

class SoleacBleService {
  SoleacBleService({FlutterReactiveBle? ble})
      : _ble = ble ?? FlutterReactiveBle();

  final FlutterReactiveBle _ble;
  final StreamController<SoleacBleStatus> _statusController =
      StreamController<SoleacBleStatus>.broadcast();
  final StreamController<SoleacTelemetry> _telemetryController =
      StreamController<SoleacTelemetry>.broadcast();

  StreamSubscription<DiscoveredDevice>? _scanSubscription;
  StreamSubscription<ConnectionStateUpdate>? _connectionSubscription;
  StreamSubscription<List<int>>? _telemetrySubscription;
  bool _connecting = false;
  bool _connected = false;

  Stream<SoleacBleStatus> get status => _statusController.stream;
  Stream<SoleacTelemetry> get telemetry => _telemetryController.stream;
  bool get isConnected => _connected;

  Future<void> setDemoMode(bool enabled) async {
    if (!_connected) {
      throw const SoleacBleException('Conectá la ESP32 antes de activar el modo demo.');
    }

    final deviceId = _activeDeviceId;
    if (deviceId == null) {
      throw const SoleacBleException('No se encontró la conexión BLE activa.');
    }

    final characteristic = QualifiedCharacteristic(
      characteristicId: soleacCommandCharacteristicUuid,
      serviceId: soleacServiceUuid,
      deviceId: deviceId,
    );
    await _ble.writeCharacteristicWithoutResponse(
      characteristic,
      value: utf8.encode(enabled ? 'demo_on' : 'demo_off'),
    );
  }

  String? _activeDeviceId;

  Future<void> connect({
    Duration scanTimeout = const Duration(seconds: 8),
    Duration connectionTimeout = const Duration(seconds: 12),
  }) async {
    if (_connecting || _connected) {
      return;
    }

    _connecting = true;

    try {
      _emit(SoleacBlePhase.checkingAdapter, 'Revisando Bluetooth');
      await _prepareBluetooth();

      _emit(SoleacBlePhase.scanning, 'Buscando detector SOLEAC...');
      final device = await _scanForSoleac(scanTimeout);

      _emit(SoleacBlePhase.connecting, 'Conectando a SOLEAC...');
      await _connectToDevice(device.id, connectionTimeout);
    } catch (error) {
      await disconnect();
      _emit(SoleacBlePhase.error, error.toString());
      rethrow;
    } finally {
      _connecting = false;
    }
  }

  Future<void> disconnect() async {
    await _scanSubscription?.cancel();
    _scanSubscription = null;

    await _telemetrySubscription?.cancel();
    _telemetrySubscription = null;

    await _connectionSubscription?.cancel();
    _connectionSubscription = null;

    _connected = false;
    _activeDeviceId = null;
    _emit(SoleacBlePhase.disconnected, 'Desconectado');
  }

  Future<void> dispose() async {
    await disconnect();
    await _statusController.close();
    await _telemetryController.close();
  }

  Future<void> _prepareBluetooth() async {
    if (Platform.isAndroid) {
      await _requestAndroidPermissions();
    }

    await _ble.statusStream
        .where((status) => status == BleStatus.ready)
        .first
        .timeout(
          const Duration(seconds: 12),
          onTimeout: () => throw const SoleacBleException(
            'Bluetooth no está listo o faltan permisos.',
          ),
        );
  }

  Future<void> _requestAndroidPermissions() async {
    final statuses = await <Permission>[
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();

    final scanDenied = statuses[Permission.bluetoothScan]?.isDenied == true ||
        statuses[Permission.bluetoothScan]?.isPermanentlyDenied == true;
    final connectDenied =
        statuses[Permission.bluetoothConnect]?.isDenied == true ||
            statuses[Permission.bluetoothConnect]?.isPermanentlyDenied == true;

    if (scanDenied || connectDenied) {
      throw const SoleacBleException(
        'Permisos Bluetooth denegados. Habilítalos en Android.',
      );
    }
  }

  Future<DiscoveredDevice> _scanForSoleac(Duration timeout) async {
    final byService = await _runScan(
      timeout: timeout,
      withServices: [soleacServiceUuid],
      acceptAnyResult: true,
    );
    if (byService != null) {
      return byService;
    }

    final byName = await _runScan(
      timeout: timeout,
      withServices: const [],
      acceptAnyResult: false,
    );
    if (byName != null) {
      return byName;
    }

    throw const SoleacBleException(
      'No se encontraron dispositivos. Intentá nuevamente.',
    );
  }

  Future<DiscoveredDevice?> _runScan({
    required Duration timeout,
    required List<Uuid> withServices,
    required bool acceptAnyResult,
  }) async {
    final completer = Completer<DiscoveredDevice?>();

    await _scanSubscription?.cancel();
    _scanSubscription = _ble.scanForDevices(withServices: withServices).listen(
      (device) {
        final matchesName = device.name.trim() == soleacDeviceName;
        if (acceptAnyResult || matchesName) {
          if (!completer.isCompleted) {
            completer.complete(device);
          }
        }
      },
      onError: (Object error) {
        if (!completer.isCompleted) {
          completer.completeError(error);
        }
      },
    );

    try {
      return await completer.future.timeout(timeout, onTimeout: () => null);
    } finally {
      await _scanSubscription?.cancel();
      _scanSubscription = null;
    }
  }

  Future<void> _connectToDevice(
    String deviceId,
    Duration connectionTimeout,
  ) async {
    final connectedCompleter = Completer<void>();
    _activeDeviceId = deviceId;

    await _connectionSubscription?.cancel();
    _connectionSubscription = _ble
        .connectToDevice(
      id: deviceId,
      connectionTimeout: connectionTimeout,
    )
        .listen(
      (update) async {
        if (update.connectionState == DeviceConnectionState.connected) {
          _connected = true;
          _emit(SoleacBlePhase.connected, 'Conectado - esperando datos');
          await _subscribeToTelemetry(deviceId);

          if (!connectedCompleter.isCompleted) {
            connectedCompleter.complete();
          }
          return;
        }

        if (update.connectionState == DeviceConnectionState.disconnected) {
          await _telemetrySubscription?.cancel();
          _telemetrySubscription = null;
          _connected = false;
          _activeDeviceId = null;

          if (!connectedCompleter.isCompleted) {
            connectedCompleter.completeError(
              const SoleacBleException('No se pudo conectar con SOLEAC.'),
            );
          } else {
            _emit(SoleacBlePhase.disconnected, 'Desconectado');
          }
        }
      },
      onError: (Object error) {
        _connected = false;
        if (!connectedCompleter.isCompleted) {
          connectedCompleter.completeError(error);
        } else {
          _emit(SoleacBlePhase.error, error.toString());
        }
      },
    );

    await connectedCompleter.future.timeout(
      connectionTimeout + const Duration(seconds: 2),
      onTimeout: () => throw const SoleacBleException(
        'Tiempo agotado conectando a SOLEAC.',
      ),
    );
  }

  Future<void> _subscribeToTelemetry(String deviceId) async {
    final characteristic = QualifiedCharacteristic(
      characteristicId: soleacTelemetryCharacteristicUuid,
      serviceId: soleacServiceUuid,
      deviceId: deviceId,
    );

    if (Platform.isAndroid) {
      try {
        await _ble.requestMtu(deviceId: deviceId, mtu: 185);
      } catch (_) {
        // Algunos telefonos no permiten negociar MTU; se continua con el valor por defecto.
      }
    }

    await _telemetrySubscription?.cancel();
    _telemetrySubscription =
        _ble.subscribeToCharacteristic(characteristic).listen(
      _handleTelemetryBytes,
      onError: (Object error) {
        _emit(SoleacBlePhase.error, 'Error leyendo telemetría BLE: $error');
      },
    );

    try {
      final initialBytes = await _ble.readCharacteristic(characteristic);
      if (initialBytes.isNotEmpty) {
        _handleTelemetryBytes(initialBytes);
      }
    } catch (_) {
      // Si la lectura puntual falla, las notificaciones siguen activas.
    }
  }

  void _handleTelemetryBytes(List<int> bytes) {
    try {
      _telemetryController.add(SoleacTelemetry.fromBleBytes(bytes));
    } catch (error) {
      if (SoleacTelemetry.looksIncompletePayload(bytes)) {
        return;
      }
      _emit(SoleacBlePhase.error, 'Dato BLE inválido: $error');
    }
  }

  void _emit(SoleacBlePhase phase, String message) {
    if (!_statusController.isClosed) {
      _statusController.add(SoleacBleStatus(phase, message));
    }
  }
}
