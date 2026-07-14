import 'dart:async';

import 'package:flutter/material.dart';

import 'soleac_ble_service.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const SoleacApp());
}

class SoleacApp extends StatelessWidget {
  const SoleacApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF006D77)),
        useMaterial3: true,
      ),
      home: const SoleacHomePage(),
    );
  }
}

class SoleacHomePage extends StatefulWidget {
  const SoleacHomePage({super.key});

  @override
  State<SoleacHomePage> createState() => _SoleacHomePageState();
}

class _SoleacHomePageState extends State<SoleacHomePage> {
  final SoleacBleService _ble = SoleacBleService();

  StreamSubscription<SoleacBleStatus>? _statusSubscription;
  StreamSubscription<SoleacTelemetry>? _telemetrySubscription;

  double intensidad = 0.0;
  double umbral = 0.0;
  String fluido = 'Desconectado';
  bool buscando = false;
  bool conectado = false;
  bool alarma = false;
  bool demo = false;
  String? error;

  @override
  void initState() {
    super.initState();

    _statusSubscription = _ble.status.listen((status) {
      if (!mounted) {
        return;
      }

      setState(() {
        conectado = status.isConnected;
        error = status.phase == SoleacBlePhase.error ? status.message : null;

        if (status.phase == SoleacBlePhase.scanning ||
            status.phase == SoleacBlePhase.checkingAdapter ||
            status.phase == SoleacBlePhase.connecting) {
          buscando = true;
          fluido = status.message;
          return;
        }

        buscando = false;

        if (status.phase == SoleacBlePhase.connected) {
          fluido = status.message;
        } else if (status.phase == SoleacBlePhase.disconnected) {
          fluido = 'Desconectado';
          intensidad = 0;
          umbral = 0;
          alarma = false;
          demo = false;
        }
      });
    });

    _telemetrySubscription = _ble.telemetry.listen((data) {
      if (!mounted) {
        return;
      }

      setState(() {
        intensidad = data.highFrequency;
        umbral = data.threshold;
        alarma = data.alarm;
        demo = data.demo;
        conectado = true;
        buscando = false;
        fluido = _estadoDesdeTelemetria(data);
      });
    });
  }

  @override
  void dispose() {
    _statusSubscription?.cancel();
    _telemetrySubscription?.cancel();
    unawaited(_ble.dispose());
    super.dispose();
  }

  Future<void> iniciarEscaneoBLE() async {
    setState(() {
      buscando = true;
      error = null;
      fluido = 'Buscando detector SOLEAC...';
    });

    try {
      await _ble.connect();
    } catch (e) {
      if (!mounted) {
        return;
      }

      setState(() {
        buscando = false;
        conectado = false;
        fluido = 'No se pudo conectar. Reintentar.';
        error = e.toString();
      });
    }
  }

  Future<void> desconectarBLE() async {
    await _ble.disconnect();
    if (!mounted) {
      return;
    }

    setState(() {
      conectado = false;
      buscando = false;
      intensidad = 0;
      umbral = 0;
      alarma = false;
      demo = false;
      fluido = 'Desconectado';
    });
  }

  String _estadoDesdeTelemetria(SoleacTelemetry data) {
    if (data.demo && data.alarm) {
      return 'Modo demo - fuga detectada';
    }
    if (data.demo) {
      return 'Modo demo - sistema normal';
    }
    if (data.alarm) {
      return 'FUGA DETECTADA';
    }
    return 'Sistema normal';
  }

  @override
  Widget build(BuildContext context) {
    final colorAlerta = alarma ? Colors.redAccent : const Color(0xFF006D77);
    final porcentajeUmbral =
        umbral <= 0 ? 0.0 : (intensidad / umbral).clamp(0.0, 1.0).toDouble();

    return Scaffold(
      appBar: AppBar(
        title: const Text(
          'SOLEAC - Monitoreo BLE',
          style: TextStyle(color: Colors.white),
        ),
        backgroundColor: const Color(0xFF006D77),
        centerTitle: true,
      ),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(20.0),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.spaceAround,
            children: [
              Card(
                elevation: 4,
                child: ListTile(
                  leading: Icon(
                    conectado ? Icons.bluetooth_connected : Icons.bluetooth,
                    color: const Color(0xFF006D77),
                  ),
                  title: const Text('Estado del Detector'),
                  subtitle: Text(
                    error == null ? fluido : '$fluido\n$error',
                    style: const TextStyle(
                      fontSize: 16,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  trailing: buscando
                      ? const CircularProgressIndicator()
                      : ElevatedButton(
                          onPressed:
                              conectado ? desconectarBLE : iniciarEscaneoBLE,
                          child: Text(conectado ? 'Salir' : 'Buscar'),
                        ),
                ),
              ),
              Column(
                children: [
                  Text(
                    '${intensidad.toStringAsFixed(1)} dB',
                    style: TextStyle(
                      fontSize: 60,
                      fontWeight: FontWeight.w900,
                      color: colorAlerta,
                    ),
                  ),
                  const Text(
                    'Senal ultrasonica (20kHz-90kHz)',
                    style: TextStyle(color: Colors.grey),
                  ),
                  const SizedBox(height: 14),
                  LinearProgressIndicator(
                    value: porcentajeUmbral,
                    minHeight: 10,
                    color: colorAlerta,
                    backgroundColor: colorAlerta.withValues(alpha: 0.16),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    'Umbral: ${umbral.toStringAsFixed(1)}',
                    style: const TextStyle(color: Colors.grey),
                  ),
                ],
              ),
              Container(
                width: double.infinity,
                padding: const EdgeInsets.symmetric(vertical: 30),
                decoration: BoxDecoration(
                  color: colorAlerta.withValues(alpha: 0.1),
                  borderRadius: BorderRadius.circular(20),
                  border: Border.all(color: colorAlerta, width: 3),
                ),
                child: Column(
                  children: [
                    Icon(
                      alarma
                          ? Icons.warning_amber_rounded
                          : Icons.check_circle_outline,
                      size: 80,
                      color: colorAlerta,
                    ),
                    const SizedBox(height: 10),
                    Text(
                      alarma ? 'FUGA DETECTADA' : 'SISTEMA NORMAL',
                      style: TextStyle(
                        fontSize: 24,
                        fontWeight: FontWeight.bold,
                        color: colorAlerta,
                      ),
                    ),
                    if (demo) ...[
                      const SizedBox(height: 8),
                      Text(
                        'MODO DEMO',
                        style: TextStyle(
                          fontWeight: FontWeight.bold,
                          color: colorAlerta,
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
