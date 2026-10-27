import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'soleac_ble_service.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const SoleacApp());
}

const Color _ink = Color(0xFF061927);
const Color _panel = Color(0xFF0B2230);
const Color _panelSoft = Color(0xFF143546);
const Color _line = Color(0xFF7DBFC6);
const Color _mint = Color(0xFF8ED4D8);
const Color _cyan = Color(0xFF67B7C6);
const Color _warning = Color(0xFFFFD166);
const Color _danger = Color(0xFFFF6B6B);
const Color _text = Color(0xFFF5F7FA);
const Color _muted = Color(0xFFC4D1D8);
const Color _lightBg = Color(0xFFEAF2F4);
const Color _lightText = Color(0xFF17323D);
const Color _lightMuted = Color(0xFF49656D);
const Color _lightPanel = Color(0xFFF9FCFC);
const Color _lightPanelSoft = Color(0xFFE5F2F3);
const Color _lightLine = Color(0xFF9AC7CC);

Color _primaryText(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark ? _text : _lightText;

Color _secondaryText(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark ? _muted : _lightMuted;

Color _softSurface(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark
        ? _panelSoft
        : _lightPanelSoft;

class SoleacApp extends StatefulWidget {
  const SoleacApp({super.key});

  @override
  State<SoleacApp> createState() => _SoleacAppState();
}

class _SoleacAppState extends State<SoleacApp> {
  bool _darkMode = true;

  @override
  void initState() {
    super.initState();
    _loadThemePreference();
  }

  Future<void> _loadThemePreference() async {
    final prefs = await SharedPreferences.getInstance();
    if (!mounted) {
      return;
    }
    setState(() {
      _darkMode = prefs.getBool(_PrefsKeys.darkMode) ?? true;
    });
  }

  Future<void> _setDarkMode(bool value) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_PrefsKeys.darkMode, value);
    if (!mounted) {
      return;
    }
    setState(() {
      _darkMode = value;
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      themeMode: _darkMode ? ThemeMode.dark : ThemeMode.light,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: _cyan,
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: _lightBg,
        navigationBarTheme: NavigationBarThemeData(
          backgroundColor: Colors.white,
          indicatorColor: _mint.withValues(alpha: 0.22),
          iconTheme: WidgetStateProperty.resolveWith(
            (states) => IconThemeData(
              color:
                  states.contains(WidgetState.selected) ? _panel : _lightText,
            ),
          ),
          labelTextStyle: WidgetStateProperty.all(
            const TextStyle(
              color: _lightText,
              fontWeight: FontWeight.w700,
            ),
          ),
        ),
        useMaterial3: true,
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: _mint,
          brightness: Brightness.dark,
          surface: _ink,
          onSurface: _text,
        ),
        scaffoldBackgroundColor: _ink,
        dividerColor: _line.withValues(alpha: 0.38),
        navigationBarTheme: NavigationBarThemeData(
          backgroundColor: _panel,
          indicatorColor: _panelSoft,
          labelTextStyle: WidgetStateProperty.all(
            const TextStyle(
              color: _text,
              fontWeight: FontWeight.w700,
            ),
          ),
          iconTheme: WidgetStateProperty.all(
            const IconThemeData(color: _text),
          ),
        ),
        useMaterial3: true,
        fontFamily: 'Roboto',
      ),
      home: SoleacHomePage(
        darkMode: _darkMode,
        onDarkModeChanged: _setDarkMode,
      ),
    );
  }
}

class SoleacHomePage extends StatefulWidget {
  const SoleacHomePage({
    required this.darkMode,
    required this.onDarkModeChanged,
    super.key,
  });

  final bool darkMode;
  final ValueChanged<bool> onDarkModeChanged;

  @override
  State<SoleacHomePage> createState() => _SoleacHomePageState();
}

class _SoleacHomePageState extends State<SoleacHomePage> {
  final SoleacBleService _ble = SoleacBleService();

  StreamSubscription<SoleacBleStatus>? _statusSubscription;
  StreamSubscription<SoleacTelemetry>? _telemetrySubscription;

  final List<SoleacLogEntry> _logs = <SoleacLogEntry>[];

  int _currentTab = 0;
  double intensidad = 0.0;
  double umbral = 0.0;
  String estado = 'Desconectado';
  String? error;
  bool buscando = false;
  bool conectado = false;
  bool alarma = false;
  bool demo = false;
  bool alertaSonora = true;
  bool notificacionesPush = false;
  bool calibracionPendiente = true;
  DateTime? ultimaTelemetria;

  @override
  void initState() {
    super.initState();
    _loadSavedState();
    _listenBle();
  }

  void _listenBle() {
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
          estado = status.message;
        } else {
          buscando = false;
          estado = status.phase == SoleacBlePhase.error
              ? 'Sin conexión'
              : status.phase == SoleacBlePhase.disconnected
                  ? 'Desconectado'
                  : status.message;
        }

        if (status.phase == SoleacBlePhase.disconnected) {
          intensidad = 0;
          umbral = 0;
          alarma = false;
          demo = false;
        }
      });

      _addLog(
        _titleForStatus(status),
        status.message,
        status.phase == SoleacBlePhase.error
            ? SoleacLogSeverity.critical
            : SoleacLogSeverity.info,
      );
    });

    _telemetrySubscription = _ble.telemetry.listen((data) {
      if (!mounted) {
        return;
      }

      final previousAlarm = alarma;
      setState(() {
        intensidad = data.highFrequency;
        umbral = data.threshold;
        alarma = data.alarm;
        demo = data.demo;
        conectado = true;
        buscando = false;
        error = null;
        ultimaTelemetria = data.receivedAt;
        estado = _estadoDesdeTelemetria(data);
      });

      if (data.alarm && !previousAlarm) {
        _addLog(
          'Fuga detectada',
          'La señal superó el umbral configurado.',
          SoleacLogSeverity.critical,
          value: '${data.highFrequency.toStringAsFixed(1)} dB',
        );
      } else if (!data.alarm && previousAlarm) {
        _addLog(
          'Sistema normalizado',
          'La lectura volvió a estar por debajo del umbral.',
          SoleacLogSeverity.success,
          value: '${data.highFrequency.toStringAsFixed(1)} dB',
        );
      } else if (_logs.isEmpty ||
          DateTime.now().difference(_logs.first.createdAt).inSeconds > 20) {
        _addLog(
          'Telemetría recibida',
          'Lectura HF ${data.highFrequency.toStringAsFixed(1)} dB.',
          SoleacLogSeverity.info,
          value: data.demo ? 'Demo' : null,
        );
      }
    });
  }

  Future<void> _loadSavedState() async {
    final prefs = await SharedPreferences.getInstance();
    final savedLogs = prefs.getStringList(_PrefsKeys.logs) ?? const <String>[];
    if (!mounted) {
      return;
    }

    setState(() {
      alertaSonora = prefs.getBool(_PrefsKeys.soundAlert) ?? true;
      notificacionesPush = prefs.getBool(_PrefsKeys.pushNotifications) ?? false;
      calibracionPendiente =
          prefs.getBool(_PrefsKeys.calibrationRequired) ?? true;
      _logs
        ..clear()
        ..addAll(
          savedLogs
              .map(SoleacLogEntry.fromJson)
              .whereType<SoleacLogEntry>()
              .toList(),
        );
    });
  }

  Future<void> _saveLogs() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setStringList(
      _PrefsKeys.logs,
      _logs.take(80).map((entry) => entry.toJson()).toList(),
    );
  }

  Future<void> _savePreference(String key, bool value) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(key, value);
  }

  Future<void> iniciarEscaneoBLE() async {
    setState(() {
      buscando = true;
      error = null;
      estado = 'Buscando detector SOLEAC...';
    });

    _addLog(
      'Búsqueda iniciada',
      'Escaneando dispositivos BLE cercanos.',
      SoleacLogSeverity.info,
    );

    try {
      await _ble.connect();
    } catch (e) {
      if (!mounted) {
        return;
      }

      setState(() {
        buscando = false;
        conectado = false;
        estado = 'Sin conexión';
        error = e.toString();
      });

      _addLog(
        'Error de conexión',
        e.toString(),
        SoleacLogSeverity.critical,
      );
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
      estado = 'Desconectado';
    });

    _addLog(
      'Conexión cerrada',
      'El enlace BLE fue desconectado manualmente.',
      SoleacLogSeverity.warning,
    );
  }

  Future<void> _setDemoMode(bool enabled) async {
    if (!conectado) {
      return;
    }
    try {
      await _ble.setDemoMode(enabled);
      _addLog(
        enabled ? 'Modo demo activado' : 'Modo demo desactivado',
        enabled
            ? 'La ESP32 enviará mediciones simuladas por BLE.'
            : 'La ESP32 retomará las mediciones de los sensores.',
        SoleacLogSeverity.info,
      );
    } catch (error) {
      _addLog('Error al cambiar el modo demo', error.toString(), SoleacLogSeverity.critical);
    }
  }

  void _addLog(
    String title,
    String message,
    SoleacLogSeverity severity, {
    String? value,
  }) {
    final entry = SoleacLogEntry(
      title: title,
      message: message,
      severity: severity,
      value: value,
      createdAt: DateTime.now(),
    );

    if (mounted) {
      setState(() {
        _logs.insert(0, entry);
        if (_logs.length > 80) {
          _logs.removeRange(80, _logs.length);
        }
      });
    } else {
      _logs.insert(0, entry);
    }

    unawaited(_saveLogs());
  }

  Future<void> _clearLogs() async {
    setState(_logs.clear);
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_PrefsKeys.logs);
  }

  Future<void> _completeCalibration() async {
    setState(() {
      calibracionPendiente = false;
    });
    await _savePreference(_PrefsKeys.calibrationRequired, false);
    _addLog(
      'Calibración completada',
      'El sensor quedó listo para funcionar.',
      SoleacLogSeverity.success,
    );
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

  String _titleForStatus(SoleacBleStatus status) {
    switch (status.phase) {
      case SoleacBlePhase.checkingAdapter:
        return 'Revisión de Bluetooth';
      case SoleacBlePhase.scanning:
        return 'Búsqueda BLE';
      case SoleacBlePhase.connecting:
        return 'Conectando';
      case SoleacBlePhase.connected:
        return 'ESP32 conectado';
      case SoleacBlePhase.disconnected:
        return 'ESP32 desconectado';
      case SoleacBlePhase.error:
        return 'Error BLE';
      case SoleacBlePhase.idle:
        return 'Sistema en espera';
    }
  }

  @override
  void dispose() {
    _statusSubscription?.cancel();
    _telemetrySubscription?.cancel();
    unawaited(_ble.dispose());
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final pages = <Widget>[
      _DashboardView(
        intensidad: intensidad,
        umbral: umbral,
        estado: estado,
        error: error,
        conectado: conectado,
        buscando: buscando,
        alarma: alarma,
        demo: demo,
        ultimaTelemetria: ultimaTelemetria,
        onPrimaryAction: conectado ? desconectarBLE : iniciarEscaneoBLE,
      ),
      _LogsView(
        logs: _logs,
        onClear: _clearLogs,
      ),
      _SettingsView(
        alertaSonora: alertaSonora,
        modoOscuro: widget.darkMode,
        notificacionesPush: notificacionesPush,
        modoDemo: demo,
        calibracionPendiente: calibracionPendiente,
        conectado: conectado,
        onSoundChanged: (value) {
          setState(() => alertaSonora = value);
          unawaited(_savePreference(_PrefsKeys.soundAlert, value));
        },
        onDarkModeChanged: widget.onDarkModeChanged,
        onPushChanged: null,
        onDemoChanged: conectado ? _setDemoMode : null,
        onCalibration: _completeCalibration,
      ),
      _DeviceView(
        conectado: conectado,
        buscando: buscando,
        estado: estado,
        intensidad: intensidad,
        umbral: umbral,
        ultimaTelemetria: ultimaTelemetria,
        onPrimaryAction: conectado ? desconectarBLE : iniciarEscaneoBLE,
      ),
    ];

    return Scaffold(
      backgroundColor: Theme.of(context).scaffoldBackgroundColor,
      body: SafeArea(
        child: AnimatedSwitcher(
          duration: const Duration(milliseconds: 220),
          child: pages[_currentTab],
        ),
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _currentTab,
        onDestinationSelected: (index) {
          setState(() => _currentTab = index);
        },
        backgroundColor: widget.darkMode ? _panel : Colors.white,
        indicatorColor: _mint.withValues(alpha: 0.18),
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.space_dashboard_outlined),
            selectedIcon: Icon(Icons.space_dashboard),
            label: 'Panel',
          ),
          NavigationDestination(
            icon: Icon(Icons.history_outlined),
            selectedIcon: Icon(Icons.history),
            label: 'Historial',
          ),
          NavigationDestination(
            icon: Icon(Icons.tune_outlined),
            selectedIcon: Icon(Icons.tune),
            label: 'Ajustes',
          ),
          NavigationDestination(
            icon: Icon(Icons.memory_outlined),
            selectedIcon: Icon(Icons.memory),
            label: 'Equipo',
          ),
        ],
      ),
    );
  }
}

class _DashboardView extends StatelessWidget {
  const _DashboardView({
    required this.intensidad,
    required this.umbral,
    required this.estado,
    required this.error,
    required this.conectado,
    required this.buscando,
    required this.alarma,
    required this.demo,
    required this.ultimaTelemetria,
    required this.onPrimaryAction,
  });

  final double intensidad;
  final double umbral;
  final String estado;
  final String? error;
  final bool conectado;
  final bool buscando;
  final bool alarma;
  final bool demo;
  final DateTime? ultimaTelemetria;
  final VoidCallback onPrimaryAction;

  @override
  Widget build(BuildContext context) {
    // La falta de conexión no es una alarma. Solo usamos rojo cuando hay
    // una fuga confirmada; los estados sin datos conservan el color neutro.
    final alertColor = alarma ? _danger : _mint;
    final systemLabel = buscando
        ? 'BUSCANDO...'
        : conectado
            ? (alarma ? 'ALERTA' : 'NORMAL')
            : 'SIN CONEXIÓN';
    final ratio =
        umbral <= 0 ? 0.0 : (intensidad / umbral).clamp(0.0, 1.0).toDouble();

    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(20, 18, 20, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _Header(
            title: 'SOLEAC',
            subtitle: conectado ? 'ESP32 conectado' : 'Monitor BLE',
            connected: conectado,
          ),
          const SizedBox(height: 22),
          _Panel(
            child: Column(
              children: [
                Row(
                  children: [
                    Expanded(
                      child: Text(
                        'ESTADO DEL\nSISTEMA',
                        style: TextStyle(
                          color: _primaryText(context),
                          fontSize: 13,
                          fontWeight: FontWeight.w800,
                          height: 1.35,
                        ),
                      ),
                    ),
                    Expanded(
                      child: Center(
                        child: Text(
                          systemLabel,
                          style: TextStyle(
                            color: alertColor,
                            fontSize: 14,
                            fontWeight: FontWeight.w900,
                          ),
                        ),
                      ),
                    ),
                    _ConnectionButton(
                      connected: conectado,
                      busy: buscando,
                      onPressed: onPrimaryAction,
                    ),
                  ],
                ),
                const SizedBox(height: 24),
                Center(
                  child: SizedBox(
                    width: 214,
                    height: 214,
                    child: CustomPaint(
                      painter: _GaugePainter(
                        value: ratio,
                        color: alertColor,
                      ),
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Text(
                            intensidad.toStringAsFixed(1),
                            style: TextStyle(
                              color: alertColor,
                              fontSize: 42,
                              fontWeight: FontWeight.w900,
                            ),
                          ),
                          Text(
                            'dB',
                            style: TextStyle(
                              color: _primaryText(context),
                              fontWeight: FontWeight.w800,
                              letterSpacing: 0,
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 20),
                _StatusBadge(
                  alarm: alarma,
                  demo: demo,
                  text: estado,
                ),
                if (error != null) ...[
                  const SizedBox(height: 14),
                  _InlineAlert(text: error!),
                ],
              ],
            ),
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              Expanded(
                child: _MetricCard(
                  title: 'Intensidad',
                  value: _signalLabel(ratio),
                  icon: Icons.graphic_eq,
                  color: alertColor,
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: _MetricCard(
                  title: 'Umbral',
                  value: conectado && umbral > 0
                      ? '${umbral.toStringAsFixed(1)} dB'
                      : '—',
                  icon: Icons.speed,
                  color: _cyan,
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          _Panel(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const _SectionTitle(label: 'Señal ultrasónica'),
                const SizedBox(height: 14),
                ClipRRect(
                  borderRadius: BorderRadius.circular(999),
                  child: LinearProgressIndicator(
                    value: ratio,
                    minHeight: 12,
                    color: alertColor,
                    backgroundColor:
                        (Theme.of(context).brightness == Brightness.dark
                                ? Colors.white
                                : _lightLine)
                            .withValues(alpha: 0.28),
                  ),
                ),
                const SizedBox(height: 12),
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    Text(
                      '20 kHz',
                      style: TextStyle(
                          color: _secondaryText(context), fontSize: 12),
                    ),
                    Text(
                      ultimaTelemetria == null
                          ? 'Sin telemetría'
                          : 'Última lectura ${_formatTime(ultimaTelemetria!)}',
                      style: TextStyle(
                          color: _secondaryText(context), fontSize: 12),
                    ),
                    Text(
                      '90 kHz',
                      style: TextStyle(
                          color: _secondaryText(context), fontSize: 12),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          _Panel(
            color: alertColor.withValues(alpha: 0.12),
            borderColor: alertColor.withValues(alpha: 0.7),
            child: Row(
              children: [
                Icon(
                  buscando
                      ? Icons.bluetooth_searching
                      : alarma
                          ? Icons.warning_amber_rounded
                          : error != null
                              ? Icons.search_off
                              : conectado
                                  ? Icons.check_circle_outline
                                  : Icons.sensors_off_outlined,
                  color: alertColor,
                  size: 42,
                ),
                const SizedBox(width: 16),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        alarma
                            ? 'FUGA DETECTADA'
                            : conectado
                                ? 'SISTEMA NORMAL'
                                : 'SISTEMA SIN CONEXIÓN',
                        style: TextStyle(
                          color: alertColor,
                          fontSize: 22,
                          fontWeight: FontWeight.w900,
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text(
                        demo
                            ? 'Modo demo activo'
                            : conectado
                                ? 'Operación en monitoreo'
                                : 'No se reciben datos',
                        style: TextStyle(color: _primaryText(context)),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  String _signalLabel(double ratio) {
    if (!conectado) {
      return '—';
    }
    if (ratio >= 0.85 || alarma) {
      return 'Alta';
    }
    if (ratio >= 0.45) {
      return 'Media';
    }
    return 'Baja';
  }
}

class _LogsView extends StatelessWidget {
  const _LogsView({
    required this.logs,
    required this.onClear,
  });

  final List<SoleacLogEntry> logs;
  final VoidCallback onClear;

  @override
  Widget build(BuildContext context) {
    final critical = logs
        .where((entry) => entry.severity == SoleacLogSeverity.critical)
        .length;
    final warnings = logs
        .where((entry) => entry.severity == SoleacLogSeverity.warning)
        .length;

    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(20, 18, 20, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const _Header(
            title: 'Historial',
            subtitle: 'Historial de eventos del sistema',
            connected: false,
            showStatus: false,
          ),
          const SizedBox(height: 18),
          Row(
            children: [
              Expanded(
                child: _MetricCard(
                  title: 'Eventos',
                  value: '${logs.length}',
                  icon: Icons.timeline,
                  color: _mint,
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: _MetricCard(
                  title: 'Críticos',
                  value: '$critical',
                  icon: Icons.priority_high,
                  color: _danger,
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          _MetricCard(
            title: 'Advertencias',
            value: '$warnings',
            icon: Icons.report_problem_outlined,
            color: _warning,
          ),
          const SizedBox(height: 18),
          Row(
            children: [
              Expanded(
                child: Text(
                  'Historial local',
                  style: TextStyle(
                    color: _primaryText(context),
                    fontSize: 18,
                    fontWeight: FontWeight.w800,
                  ),
                ),
              ),
              TextButton.icon(
                onPressed: logs.isEmpty ? null : onClear,
                icon: const Icon(Icons.delete_sweep_outlined),
                label: const Text('Limpiar historial'),
              ),
            ],
          ),
          const SizedBox(height: 8),
          if (logs.isEmpty)
            const _EmptyState()
          else
            ...logs.map((entry) => _LogTile(entry: entry)),
        ],
      ),
    );
  }
}

class _SettingsView extends StatelessWidget {
  const _SettingsView({
    required this.alertaSonora,
    required this.modoOscuro,
    required this.notificacionesPush,
    required this.modoDemo,
    required this.calibracionPendiente,
    required this.conectado,
    required this.onSoundChanged,
    required this.onDarkModeChanged,
    required this.onPushChanged,
    required this.onDemoChanged,
    required this.onCalibration,
  });

  final bool alertaSonora;
  final bool modoOscuro;
  final bool notificacionesPush;
  final bool modoDemo;
  final bool calibracionPendiente;
  final bool conectado;
  final ValueChanged<bool> onSoundChanged;
  final ValueChanged<bool> onDarkModeChanged;
  final ValueChanged<bool>? onPushChanged;
  final ValueChanged<bool>? onDemoChanged;
  final VoidCallback onCalibration;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(20, 18, 20, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const _Header(
            title: 'Configuración',
            subtitle: 'Preferencias y calibración',
            connected: false,
            showStatus: false,
          ),
          const SizedBox(height: 20),
          _Panel(
            child: Column(
              children: [
                _PreferenceSwitch(
                  icon: Icons.notifications_active_outlined,
                  title: 'Alerta sonora',
                  subtitle:
                      'Sonido de aviso cuando se detectan niveles críticos',
                  value: alertaSonora,
                  onChanged: onSoundChanged,
                ),
                const Divider(color: _line),
                _PreferenceSwitch(
                  icon: Icons.dark_mode_outlined,
                  title: 'Modo oscuro',
                  subtitle: 'Usar un tema oscuro de alto contraste',
                  value: modoOscuro,
                  onChanged: onDarkModeChanged,
                ),
                const Divider(color: _line),
                _PreferenceSwitch(
                  icon: Icons.science_outlined,
                  title: 'Modo demo',
                  subtitle: conectado
                      ? 'Mostrar mediciones simuladas de la ESP32'
                      : 'Conectá la ESP32 para habilitarlo',
                  value: modoDemo,
                  onChanged: onDemoChanged,
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          if (!conectado)
            _Panel(
              borderColor: _cyan,
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Icon(Icons.sensors_off_outlined, color: _cyan),
                  SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          'Sin sensor conectado',
                          style: TextStyle(
                            color: _primaryText(context),
                            fontWeight: FontWeight.w800,
                            fontSize: 16,
                          ),
                        ),
                        SizedBox(height: 8),
                        Text(
                          'Conectá el ESP32 para habilitar la calibración y recibir telemetría.',
                          style: TextStyle(
                              color: _secondaryText(context), height: 1.35),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            )
          else
            _Panel(
              borderColor: calibracionPendiente ? _warning : _mint,
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Icon(
                    calibracionPendiente
                        ? Icons.info_outline
                        : Icons.verified_outlined,
                    color: calibracionPendiente ? _warning : _mint,
                  ),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          calibracionPendiente
                              ? 'Calibración requerida'
                              : 'Sensor calibrado',
                          style: TextStyle(
                            color: _primaryText(context),
                            fontWeight: FontWeight.w800,
                            fontSize: 16,
                          ),
                        ),
                        const SizedBox(height: 8),
                        Text(
                          calibracionPendiente
                              ? 'Colocá el sensor en aire limpio antes de iniciar la referencia de base.'
                              : 'La calibración quedó guardada en este teléfono.',
                          style: TextStyle(
                              color: _secondaryText(context), height: 1.35),
                        ),
                        const SizedBox(height: 12),
                        FilledButton.icon(
                          onPressed:
                              calibracionPendiente ? onCalibration : null,
                          icon: const Icon(Icons.play_arrow),
                          label: const Text('Iniciar calibración'),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }
}

class _DeviceView extends StatelessWidget {
  const _DeviceView({
    required this.conectado,
    required this.buscando,
    required this.estado,
    required this.intensidad,
    required this.umbral,
    required this.ultimaTelemetria,
    required this.onPrimaryAction,
  });

  final bool conectado;
  final bool buscando;
  final String estado;
  final double intensidad;
  final double umbral;
  final DateTime? ultimaTelemetria;
  final VoidCallback onPrimaryAction;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(20, 18, 20, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _Header(
            title: 'ESP32',
            subtitle: conectado ? 'Conexión activa' : 'Sin conexión',
            connected: conectado,
          ),
          const SizedBox(height: 20),
          _Panel(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Icon(
                      conectado ? Icons.wifi_tethering : Icons.wifi_off,
                      color: conectado ? _mint : _danger,
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(
                        estado,
                        style: TextStyle(
                          color: _primaryText(context),
                          fontSize: 18,
                          fontWeight: FontWeight.w800,
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 18),
                _ConnectionButton(
                  connected: conectado,
                  busy: buscando,
                  expanded: true,
                  onPressed: onPrimaryAction,
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          _Panel(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const _SectionTitle(label: 'Telemetría'),
                const SizedBox(height: 14),
                _DeviceRow(
                  label: 'Intensidad HF',
                  value: '${intensidad.toStringAsFixed(1)} dB',
                ),
                _DeviceRow(
                  label: 'Umbral',
                  value: '${umbral.toStringAsFixed(1)} dB',
                ),
                _DeviceRow(
                  label: 'Última lectura',
                  value: ultimaTelemetria == null
                      ? '--:--:--'
                      : _formatTime(ultimaTelemetria!),
                ),
                const _DeviceRow(
                  label: 'Servicio BLE',
                  value: '12345678-...-def0',
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _Header extends StatelessWidget {
  const _Header({
    required this.title,
    required this.subtitle,
    required this.connected,
    this.showStatus = true,
  });

  final String title;
  final String subtitle;
  final bool connected;
  final bool showStatus;

  @override
  Widget build(BuildContext context) {
    final titleColor =
        Theme.of(context).brightness == Brightness.dark ? _text : _lightText;

    return Row(
      children: [
        const Icon(Icons.sensors, color: _mint, size: 22),
        const SizedBox(width: 10),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                title,
                style: TextStyle(
                  color: titleColor,
                  fontSize: 24,
                  fontWeight: FontWeight.w900,
                ),
              ),
              Text(
                subtitle.toUpperCase(),
                style: const TextStyle(
                  color: _mint,
                  fontSize: 11,
                  fontWeight: FontWeight.w700,
                  letterSpacing: 0,
                ),
              ),
            ],
          ),
        ),
        if (showStatus)
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
            decoration: BoxDecoration(
              color: (connected ? _mint : _danger).withValues(alpha: 0.12),
              borderRadius: BorderRadius.circular(999),
              border: Border.all(
                color: (connected ? _mint : _danger).withValues(alpha: 0.45),
              ),
            ),
            child: Row(
              children: [
                Icon(
                  Icons.circle,
                  color: connected ? _mint : _danger,
                  size: 8,
                ),
                const SizedBox(width: 7),
                Text(
                  connected ? 'ONLINE' : 'OFFLINE',
                  style: TextStyle(
                    color: _primaryText(context),
                    fontSize: 11,
                    fontWeight: FontWeight.w800,
                  ),
                ),
              ],
            ),
          ),
      ],
    );
  }
}

class _Panel extends StatelessWidget {
  const _Panel({
    required this.child,
    this.color,
    this.borderColor,
  });

  final Widget child;
  final Color? color;
  final Color? borderColor;

  @override
  Widget build(BuildContext context) {
    final isDark = Theme.of(context).brightness == Brightness.dark;
    final surface = color ?? (isDark ? _panel : _lightPanel);
    final outline = borderColor ?? (isDark ? _line : _lightLine);
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(18),
      decoration: BoxDecoration(
        color: surface,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(
          color: outline.withValues(alpha: isDark ? 0.58 : 0.75),
        ),
        boxShadow: [
          BoxShadow(
            color: isDark
                ? _cyan.withValues(alpha: 0.08)
                : Colors.black.withValues(alpha: 0.2),
            blurRadius: isDark ? 22 : 18,
            offset: const Offset(0, 8),
          ),
        ],
      ),
      child: child,
    );
  }
}

class _SectionTitle extends StatelessWidget {
  const _SectionTitle({
    required this.label,
  });

  final String label;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(
          child: Text(
            label.toUpperCase(),
            style: TextStyle(
              color: _primaryText(context),
              fontSize: 13,
              fontWeight: FontWeight.w800,
              letterSpacing: 0,
            ),
          ),
        ),
      ],
    );
  }
}

class _ConnectionButton extends StatelessWidget {
  const _ConnectionButton({
    required this.connected,
    required this.busy,
    required this.onPressed,
    this.expanded = false,
  });

  final bool connected;
  final bool busy;
  final VoidCallback onPressed;
  final bool expanded;

  @override
  Widget build(BuildContext context) {
    final child = FilledButton.icon(
      onPressed: busy ? null : onPressed,
      icon: busy
          ? const SizedBox(
              width: 18,
              height: 18,
              child: CircularProgressIndicator(strokeWidth: 2),
            )
          : Icon(connected ? Icons.logout : Icons.bluetooth_searching),
      label: Text(
        busy
            ? 'Buscando'
            : connected
                ? 'Salir'
                : 'Buscar',
      ),
    );

    if (!expanded) {
      return child;
    }
    return SizedBox(width: double.infinity, child: child);
  }
}

class _StatusBadge extends StatelessWidget {
  const _StatusBadge({
    required this.alarm,
    required this.demo,
    required this.text,
  });

  final bool alarm;
  final bool demo;
  final String text;

  @override
  Widget build(BuildContext context) {
    final color = alarm ? _danger : _mint;
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: _softSurface(context),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Column(
        children: [
          Icon(
            text.toLowerCase().contains('buscando')
                ? Icons.bluetooth_searching
                : alarm
                    ? Icons.warning_amber_rounded
                    : text.toLowerCase().contains('desconect') ||
                            text.toLowerCase().contains('sin conexión')
                        ? Icons.sensors_off_outlined
                        : Icons.check_circle_outline,
            color: color,
            size: 26,
          ),
          const SizedBox(height: 8),
          Text(
            text.toUpperCase(),
            textAlign: TextAlign.center,
            style: TextStyle(
              color: _primaryText(context),
              fontWeight: FontWeight.w900,
              fontSize: 16,
            ),
          ),
          if (demo) ...[
            const SizedBox(height: 4),
            const Text(
              'MODO DEMO',
              style: TextStyle(color: _warning, fontWeight: FontWeight.w800),
            ),
          ],
        ],
      ),
    );
  }
}

class _InlineAlert extends StatelessWidget {
  const _InlineAlert({required this.text});

  final String text;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: _danger.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: _danger.withValues(alpha: 0.5)),
      ),
      child: Text(
        text,
        style: TextStyle(color: _primaryText(context), height: 1.35),
      ),
    );
  }
}

class _MetricCard extends StatelessWidget {
  const _MetricCard({
    required this.title,
    required this.value,
    required this.icon,
    required this.color,
  });

  final String title;
  final String value;
  final IconData icon;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return _Panel(
      child: ConstrainedBox(
        constraints: const BoxConstraints(minHeight: 70),
        child: Row(
          children: [
            Container(
              width: 42,
              height: 42,
              decoration: BoxDecoration(
                color: color.withValues(alpha: 0.16),
                borderRadius: BorderRadius.circular(8),
              ),
              child: Icon(icon, color: color),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    title.toUpperCase(),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      color: _secondaryText(context),
                      fontSize: 10,
                      fontWeight: FontWeight.w800,
                    ),
                  ),
                  const SizedBox(height: 4),
                  Text(
                    value,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      color: _primaryText(context),
                      fontSize: 18,
                      fontWeight: FontWeight.w900,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _PreferenceSwitch extends StatelessWidget {
  const _PreferenceSwitch({
    required this.icon,
    required this.title,
    required this.subtitle,
    required this.value,
    required this.onChanged,
  });

  final IconData icon;
  final String title;
  final String subtitle;
  final bool value;
  final ValueChanged<bool>? onChanged;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Row(
        children: [
          Container(
            width: 42,
            height: 42,
            decoration: BoxDecoration(
              color: _softSurface(context),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Icon(icon, color: _mint),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: TextStyle(
                    color: _primaryText(context),
                    fontWeight: FontWeight.w800,
                    fontSize: 16,
                  ),
                ),
                Text(
                  subtitle,
                  style:
                      TextStyle(color: _secondaryText(context), fontSize: 12),
                ),
              ],
            ),
          ),
          Switch(
            value: value,
            activeThumbColor: _mint,
            onChanged: onChanged,
          ),
        ],
      ),
    );
  }
}

class _LogTile extends StatelessWidget {
  const _LogTile({required this.entry});

  final SoleacLogEntry entry;

  @override
  Widget build(BuildContext context) {
    final color = entry.severity.color;
    return Padding(
      padding: const EdgeInsets.only(bottom: 10),
      child: Container(
        width: double.infinity,
        padding: const EdgeInsets.all(14),
        decoration: BoxDecoration(
          color: Theme.of(context).brightness == Brightness.dark
              ? _panel
              : _lightPanel,
          borderRadius: BorderRadius.circular(8),
          border: Border(
            left: BorderSide(color: color, width: 4),
          ),
        ),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Container(
              width: 42,
              height: 42,
              decoration: BoxDecoration(
                color: color.withValues(alpha: 0.14),
                borderRadius: BorderRadius.circular(6),
              ),
              child: Icon(entry.severity.icon, color: color),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(
                          entry.title,
                          style: TextStyle(
                            color: _primaryText(context),
                            fontSize: 16,
                            fontWeight: FontWeight.w900,
                          ),
                        ),
                      ),
                      Text(
                        _formatTime(entry.createdAt),
                        style: const TextStyle(
                          color: _mint,
                          fontSize: 11,
                          fontWeight: FontWeight.w800,
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 6),
                  Text(
                    entry.message,
                    style:
                        TextStyle(color: _primaryText(context), height: 1.35),
                  ),
                  if (entry.value != null) ...[
                    const SizedBox(height: 6),
                    Text(
                      entry.value!,
                      style: TextStyle(
                        color: color,
                        fontWeight: FontWeight.w900,
                        fontSize: 12,
                      ),
                    ),
                  ],
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _EmptyState extends StatelessWidget {
  const _EmptyState();

  @override
  Widget build(BuildContext context) {
    return _Panel(
      child: Column(
        children: [
          Icon(Icons.receipt_long_outlined,
              color: _secondaryText(context), size: 42),
          SizedBox(height: 10),
          Text(
            'Sin eventos guardados',
            style: TextStyle(
              color: _primaryText(context),
              fontWeight: FontWeight.w800,
              fontSize: 16,
            ),
          ),
          SizedBox(height: 4),
          Text(
            'Los registros aparecerán al buscar, conectar o recibir telemetría.',
            textAlign: TextAlign.center,
            style: TextStyle(color: _secondaryText(context)),
          ),
        ],
      ),
    );
  }
}

class _DeviceRow extends StatelessWidget {
  const _DeviceRow({
    required this.label,
    required this.value,
  });

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 10),
      child: Row(
        children: [
          Expanded(
            child: Text(
              label,
              style: TextStyle(color: _secondaryText(context)),
            ),
          ),
          Flexible(
            child: Text(
              value,
              textAlign: TextAlign.right,
              style: TextStyle(
                color: _primaryText(context),
                fontWeight: FontWeight.w800,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _GaugePainter extends CustomPainter {
  const _GaugePainter({
    required this.value,
    required this.color,
  });

  final double value;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    final stroke = size.width * 0.08;
    final rect = Offset(stroke / 2, stroke / 2) &
        Size(size.width - stroke, size.height - stroke);
    final background = Paint()
      ..color = _line.withValues(alpha: 0.75)
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round;
    final foreground = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round;

    canvas.drawArc(rect, 2.25, 5.1, false, background);
    canvas.drawArc(rect, 2.25, 5.1 * value, false, foreground);
  }

  @override
  bool shouldRepaint(covariant _GaugePainter oldDelegate) {
    return oldDelegate.value != value || oldDelegate.color != color;
  }
}

class SoleacLogEntry {
  const SoleacLogEntry({
    required this.title,
    required this.message,
    required this.severity,
    required this.createdAt,
    this.value,
  });

  final String title;
  final String message;
  final SoleacLogSeverity severity;
  final DateTime createdAt;
  final String? value;

  String toJson() {
    return jsonEncode({
      'title': title,
      'message': message,
      'severity': severity.name,
      'createdAt': createdAt.toIso8601String(),
      'value': value,
    });
  }

  static SoleacLogEntry? fromJson(String source) {
    try {
      final decoded = jsonDecode(source);
      if (decoded is! Map<String, dynamic>) {
        return null;
      }
      return SoleacLogEntry(
        title: decoded['title']?.toString() ?? 'Evento',
        message: decoded['message']?.toString() ?? '',
        severity: SoleacLogSeverity.values.firstWhere(
          (item) => item.name == decoded['severity'],
          orElse: () => SoleacLogSeverity.info,
        ),
        createdAt: DateTime.tryParse(decoded['createdAt']?.toString() ?? '') ??
            DateTime.now(),
        value: decoded['value']?.toString(),
      );
    } catch (_) {
      return null;
    }
  }
}

enum SoleacLogSeverity {
  info,
  success,
  warning,
  critical,
}

extension SoleacLogSeverityStyle on SoleacLogSeverity {
  Color get color {
    switch (this) {
      case SoleacLogSeverity.info:
        return _cyan;
      case SoleacLogSeverity.success:
        return _mint;
      case SoleacLogSeverity.warning:
        return _warning;
      case SoleacLogSeverity.critical:
        return _danger;
    }
  }

  IconData get icon {
    switch (this) {
      case SoleacLogSeverity.info:
        return Icons.info_outline;
      case SoleacLogSeverity.success:
        return Icons.check_circle_outline;
      case SoleacLogSeverity.warning:
        return Icons.report_problem_outlined;
      case SoleacLogSeverity.critical:
        return Icons.warning_amber_rounded;
    }
  }
}

class _PrefsKeys {
  const _PrefsKeys._();

  static const logs = 'soleac_logs';
  static const soundAlert = 'soleac_sound_alert';
  static const darkMode = 'soleac_dark_mode';
  static const pushNotifications = 'soleac_push_notifications';
  static const calibrationRequired = 'soleac_calibration_required';
}

String _formatTime(DateTime value) {
  String two(int number) => number.toString().padLeft(2, '0');
  return '${two(value.hour)}:${two(value.minute)}:${two(value.second)}';
}
