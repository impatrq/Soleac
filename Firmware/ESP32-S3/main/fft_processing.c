/*
 * SOLEAC - procesamiento de audio y detección ultrasónica
 *
 * Todo el flujo permanece en este archivo: captura I2S/PDM, análisis FFT,
 * calibración, sensores auxiliares, estado compartido, modo demo y alarmas.
 * Las secciones están ordenadas para poder localizar rápidamente cada tipo
 * de problema sin tener que saltar entre módulos.
 */

#include "fft_processing.h"

#include <math.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include("esp_dsp.h")
#define SOLEAC_USE_ESP_DSP 1
#include "esp_dsp.h"
#else
#define SOLEAC_USE_ESP_DSP 0
#endif
#if __has_include("driver/i2s_pdm.h")
#define SOLEAC_USE_I2S_PDM_DRIVER 1
#include "driver/i2s_pdm.h"
#else
#define SOLEAC_USE_I2S_PDM_DRIVER 0
#endif

/* ============================================================
 * CONFIGURACIÓN Y COMPATIBILIDAD ESP-IDF
 * ============================================================ */

// Captura PDM RAW para conservar la banda ultrasónica del MEMS.
// En ESP-IDF 6.x el driver PDM RAW genera en el pin CLK dos veces el
// sample_rate_hz configurado. Para obtener 3.072 MHz en el pin del SPH0641
// se configura 1.536 MHz en el driver. Con 16 bits PDM por muestra PCM
// obtenemos 192 kHz efectivos.
#define SOLEAC_USE_PDM_RAW 1

#define TAG "FFT"

#define PIN_BUZZER GPIO_NUM_NC
#define PIN_LED GPIO_NUM_NC
#define PIN_BOOT_BUTTON GPIO_NUM_0
// Prueba solicitada intercambiando los roles: CLK=GPIO39 y DATA=GPIO38.
#define I2S_CLK_PIN GPIO_NUM_39
#define I2S_DATA_PIN GPIO_NUM_38
#define PDM_SENSOR_SLOT_PREFERRED I2S_PDM_SLOT_LEFT
// GPIO9/10/11 pertenecen al bus de la microSD integrado en la Waveshare.
// GPIO17 y GPIO18 están expuestos en el header y son entradas ADC2 en el S3.
#define MQ9_ADC_GPIO GPIO_NUM_17
#define MA40_ADC_GPIO GPIO_NUM_18
// El MA40S4R no está instalado actualmente. Cambiar a 1 al conectarlo.
#define MA40_SENSOR_INSTALLED 1
#define SAMPLE_RATE_HZ 192000
// FFT de 256 puntos: mantiene resolución suficiente para ubicar 20-55 kHz
// y evita que el procesamiento monopolice CPU1 durante el muestreo.
#define BUFFER_SAMPLES 256
#define DMA_BUFFER_COUNT 6
#define CALIBRATION_MS 4000
#define MIN_ULTRASONIC_THRESHOLD 8.0f
#define MIN_ULTRASONIC_MARGIN 15.0f
#define ULTRASONIC_THRESHOLD_MULTIPLIER 2.0f
#define ULTRASONIC_NOISE_SIGMA_MULTIPLIER 3.0f
#define ULTRASONIC_SEARCH_FREQ_MIN_HZ 20000.0f
#define ULTRASONIC_SEARCH_FREQ_MAX_HZ 80000.0f
#define BASELINE_UPDATE_ALPHA 0.004f
#define NOISE_UPDATE_ALPHA 0.004f
#define CALIBRATION_ALPHA 0.05f
#define DEMO_HOLD_MS 1500
#define AUDIO_IDLE_DELAY_MS 20
#define AUDIO_I2S_TIMEOUT_MS 100
#define PDM_SENSOR_WAKEUP_MS 20U
#define MEMS_NO_DATA_TIMEOUT_MS 100U
#define MQ9_READ_MS 250
#define MA40_READ_MS 50
#define MQ9_GAS_ON_PCT 25.0f
#define MQ9_GAS_OFF_PCT 18.0f
// Envolvente esperada del detector ~130-170 mV con ráfagas de 0.5 V (modelo); ajustar
// con lo que muestre el log "MA40 ... max_1s" de la placa real.
#define MA40_MIN_EVENT_MARGIN_MV 40.0f
#define MA40_NOISE_MULTIPLIER 4.0f
#define MA40_RELEASE_RATIO 0.60f
#define MQ9_DISCONNECTED_RAW 4090
#define MQ9_ADC_ATTEN ADC_ATTEN_DB_12
#define MQ9_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define MQ9_ADC_MAX_RAW 4095.0f
#define I2S_PORT I2S_NUM_0
#define FFT_SIZE BUFFER_SAMPLES
#define FFT_BIN_HZ ((float)SAMPLE_RATE_HZ / (float)FFT_SIZE)
#define PDM_SENSOR_CLOCK_HZ 3072000
#define PDM_DRIVER_RATE_HZ (PDM_SENSOR_CLOCK_HZ / 2)
// El arranque escalonado queda disponible como opción experimental. En esta
// placa/versión de ESP-IDF la reconfiguración en caliente dejó el PDM en cero,
// por lo que se usa directamente el reloj ultrasónico, que antes entregaba
// bloques válidos.
#define PDM_STAGED_STARTUP_ENABLE 0
#define PDM_STANDARD_SENSOR_CLOCK_HZ 2400000
#define PDM_STANDARD_DRIVER_RATE_HZ (PDM_STANDARD_SENSOR_CLOCK_HZ / 2)
#define PDM_STANDARD_STARTUP_DELAY_MS 100
#define PDM_ULTRASONIC_SETTLE_DELAY_MS 20
#define PDM_RAW_DECIMATION 16
#define PDM_RAW_BYTES_PER_BLOCK ((FFT_SIZE * PDM_RAW_DECIMATION) / 8)
// Diagnóstico del flujo RAW. Un PDM sano no permanece fijado en 0 o 1 y
// presenta transiciones con regularidad. Estos límites detectan DATA abierta
// sin bloquear señales de audio válidas de baja amplitud.
#define PDM_MIN_ONES_PERCENT 5.0f
#define PDM_MAX_ONES_PERCENT 95.0f
#define PDM_MIN_TRANSITIONS_PERCENT 10.0f
#define PDM_VALID_BLOCKS_TO_CONFIRM 16U
#define SEARCH_BIN_START ((int)((ULTRASONIC_SEARCH_FREQ_MIN_HZ / FFT_BIN_HZ) + 0.5f))
#define SEARCH_BIN_END ((int)((ULTRASONIC_SEARCH_FREQ_MAX_HZ / FFT_BIN_HZ) + 0.5f))

#if SOLEAC_USE_I2S_PDM_DRIVER && !defined(I2S_CHANNEL_DEFAULT_CONFIG) && defined(I2S_CHAN_DEFAULT_CONFIG)
#define I2S_CHANNEL_DEFAULT_CONFIG I2S_CHAN_DEFAULT_CONFIG
#endif
#if SOLEAC_USE_I2S_PDM_DRIVER && defined(I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG)
#define SOLEAC_PDM_RX_SLOT_DEFAULT_CONFIG(bits, mode) I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(bits, mode)
#elif SOLEAC_USE_I2S_PDM_DRIVER && defined(I2S_PDM_RX_SLOT_DEFAULT_CONFIG)
#define SOLEAC_PDM_RX_SLOT_DEFAULT_CONFIG(bits, mode) I2S_PDM_RX_SLOT_DEFAULT_CONFIG(bits, mode)
#elif SOLEAC_USE_I2S_PDM_DRIVER
#error "No compatible I2S PDM RX slot configuration macro was found."
#endif
#if SOLEAC_USE_I2S_PDM_DRIVER && SOLEAC_USE_PDM_RAW && defined(I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG)
#define SOLEAC_PDM_RX_RAW_SLOT_CONFIG(bits, mode) I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(bits, mode)
#elif SOLEAC_USE_I2S_PDM_DRIVER && SOLEAC_USE_PDM_RAW
#error "No compatible I2S PDM RAW slot configuration macro was found."
#endif

#if SOLEAC_USE_PDM_RAW
static i2s_pdm_slot_mask_t pdm_selected_slot = PDM_SENSOR_SLOT_PREFERRED;
static uint32_t pdm_descartar_durante(uint32_t duration_ms);
static bool validar_bloque_pdm(
    const uint16_t *pdm,
    size_t bytes,
    float *unos_pct_out,
    float *transiciones_pct_out);
#endif
static gpio_num_t pdm_active_clk_pin = I2S_CLK_PIN;
static gpio_num_t pdm_active_data_pin = I2S_DATA_PIN;

/* ============================================================
 * ESTADO PRIVADO Y BUFFERS DE TRABAJO
 * ============================================================ */

static detector_state_t g_state = {
    .umbral_ultrasonido = MIN_ULTRASONIC_THRESHOLD,
};
static SemaphoreHandle_t telemetry_mutex = NULL;
static bool alarma_ultra_reportada = false;
static uint32_t inicio_demo_ms = 0;
static uint32_t boton_demo_presionado_ms = 0;
static uint32_t ultimo_mq2_read_ms = 0;
static uint32_t ultimo_ma40_read_ms = 0;
static bool boton_demo_consumido = false;
static adc_oneshot_unit_handle_t mq2_adc_handle = NULL;
static adc_cali_handle_t mq2_cali_handle = NULL;
static adc_unit_t mq2_adc_unit = ADC_UNIT_1;
static adc_channel_t mq2_adc_channel = ADC_CHANNEL_0;
static bool mq2_adc_disponible = false;
static bool mq2_adc_calibrado = false;
static adc_oneshot_unit_handle_t ma40_adc_handle = NULL;
static adc_cali_handle_t ma40_cali_handle = NULL;
static adc_unit_t ma40_adc_unit = ADC_UNIT_1;
static adc_channel_t ma40_adc_channel = ADC_CHANNEL_0;
static bool ma40_adc_disponible = false;
static bool ma40_adc_calibrado = false;
static float ma40_baseline_mv = 0.0f;
static float ma40_ruido_mv = 20.0f;
static bool ma40_baseline_lista = false;
#if SOLEAC_USE_I2S_PDM_DRIVER
static i2s_chan_handle_t rx_chan;
#endif
#if SOLEAC_USE_ESP_DSP
static __attribute__((aligned(16))) float fft_input_complex[FFT_SIZE * 2];
static __attribute__((aligned(16))) float fft_window[FFT_SIZE];
#endif

/* ============================================================
 * HARDWARE BÁSICO: GPIO Y SALIDAS DE ALARMA
 * ============================================================ */

static void soleac_gpio_init(void)
{
    if (PIN_BUZZER != GPIO_NUM_NC) {
        ESP_ERROR_CHECK(gpio_reset_pin(PIN_BUZZER));
        ESP_ERROR_CHECK(gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(PIN_BUZZER, 0));
    }

    if (PIN_LED != GPIO_NUM_NC) {
        ESP_ERROR_CHECK(gpio_reset_pin(PIN_LED));
        ESP_ERROR_CHECK(gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(PIN_LED, 0));
    }

    ESP_ERROR_CHECK(gpio_reset_pin(PIN_BOOT_BUTTON));
    ESP_ERROR_CHECK(gpio_set_direction(PIN_BOOT_BUTTON, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_BOOT_BUTTON, GPIO_PULLUP_ONLY));
}

static bool soleac_gpio_boot_button_pressed(void)
{
    return gpio_get_level(PIN_BOOT_BUTTON) == 0;
}

static esp_err_t soleac_gpio_set_alarm_outputs(bool active)
{
    if (PIN_BUZZER != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(PIN_BUZZER, active ? 1 : 0), TAG, "set buzzer");
    }
    if (PIN_LED != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(PIN_LED, active ? 1 : 0), TAG, "set led");
    }
    return ESP_OK;
}

/* ============================================================
 * SENSORES ADC: MQ-9 Y MA40S4R
 * ============================================================ */

static esp_err_t mq2_adc_init(void)
{
    esp_err_t ret = adc_oneshot_io_to_channel(MQ9_ADC_GPIO, &mq2_adc_unit, &mq2_adc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQ-9: GPIO%d no es ADC valido (%s)", MQ9_ADC_GPIO, esp_err_to_name(ret));
        return ret;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = mq2_adc_unit,
    };

    ret = adc_oneshot_new_unit(&unit_cfg, &mq2_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQ-9: no se pudo crear ADC unit %d (%s)", mq2_adc_unit, esp_err_to_name(ret));
        return ret;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = MQ9_ADC_ATTEN,
        .bitwidth = MQ9_ADC_BITWIDTH,
    };

    ret = adc_oneshot_config_channel(mq2_adc_handle, mq2_adc_channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQ-9: no se pudo configurar canal ADC %d (%s)", mq2_adc_channel, esp_err_to_name(ret));
        return ret;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = mq2_adc_unit,
        .chan = mq2_adc_channel,
        .atten = MQ9_ADC_ATTEN,
        .bitwidth = MQ9_ADC_BITWIDTH,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &mq2_cali_handle);
    if (ret == ESP_OK) {
        mq2_adc_calibrado = true;
    } else {
        ESP_LOGW(TAG, "MQ-9: ADC sin calibracion en mV (%s), se usara raw", esp_err_to_name(ret));
    }
#endif

    mq2_adc_disponible = true;
    ESP_LOGI(
        TAG,
        "MQ-9 ADC listo: AO=GPIO%d unit=%d channel=%d atten=12dB",
        MQ9_ADC_GPIO,
        mq2_adc_unit,
        mq2_adc_channel);
    return ESP_OK;
}

static void mq2_adc_update(uint32_t now_ms)
{
    if (!mq2_adc_disponible || (now_ms - ultimo_mq2_read_ms) < MQ9_READ_MS) {
        return;
    }
    ultimo_mq2_read_ms = now_ms;

    int raw = 0;
    int mv = 0;
    esp_err_t ret = adc_oneshot_read(mq2_adc_handle, mq2_adc_channel, &raw);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MQ-9: fallo lectura ADC (%s)", esp_err_to_name(ret));
        return;
    }

    if (mq2_adc_calibrado) {
        ret = adc_cali_raw_to_voltage(mq2_cali_handle, raw, &mv);
        if (ret != ESP_OK) {
            mv = 0;
        }
    }

    float raw_f = (float)raw;
    // Un ADC clavado en el maximo no permite distinguir un AO saturado de una
    // entrada sin la referencia esperada. No reportarlo como gas evita una
    // alarma falsa mientras se corrige el cableado/divisor del MQ-9.
    bool conectado = raw >= 0 && raw < MQ9_DISCONNECTED_RAW;
    float pct = conectado ? (raw_f * 100.0f) / MQ9_ADC_MAX_RAW : 0.0f;
    if (pct > 100.0f) {
        pct = 100.0f;
    }

    int gas_evento = 0;   // +1: empieza a detectar gas, -1: se normaliza
    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);
    g_state.mq2_raw = raw_f;
    g_state.mq2_mv = (float)mv;
    g_state.mq2_pct = pct;
    g_state.mq2_conectado = conectado;
    if (!conectado) {
        g_state.mq2_gas_detectado = false;
    } else if (!g_state.mq2_gas_detectado && pct >= MQ9_GAS_ON_PCT) {
        g_state.mq2_gas_detectado = true;
        gas_evento = 1;
    } else if (g_state.mq2_gas_detectado && pct <= MQ9_GAS_OFF_PCT) {
        g_state.mq2_gas_detectado = false;
        gas_evento = -1;
    }
    xSemaphoreGive(telemetry_mutex);

    // Log de prueba: una línea por segundo con el máximo del último segundo.
    static uint32_t mq2_ultimo_log_ms = 0;
    static int mq2_raw_max_1s = 0;
    if (raw > mq2_raw_max_1s) {
        mq2_raw_max_1s = raw;
    }
    if (gas_evento > 0) {
        ESP_LOGW(TAG, "MQ-9: GAS DETECTADO (%.1f%% >= %.0f%%) raw=%d mv=%d", pct, MQ9_GAS_ON_PCT, raw, mv);
    } else if (gas_evento < 0) {
        ESP_LOGI(TAG, "MQ-9: gas normalizado (%.1f%%)", pct);
    }
    if ((now_ms - mq2_ultimo_log_ms) >= 1000U) {
        mq2_ultimo_log_ms = now_ms;
        ESP_LOGI(TAG, "MQ-9 GPIO%d: raw=%d mv=%d pct=%.1f max_1s=%d%s", MQ9_ADC_GPIO, raw, mv, pct,
                 mq2_raw_max_1s, conectado ? "" : " SATURADO/desconectado");
        mq2_raw_max_1s = 0;
    }

    static bool mq2_desconectado_reportado = false;
    if (!conectado && !mq2_desconectado_reportado) {
        ESP_LOGW(TAG, "MQ-9 desconectado o salida saturada: raw=%d mv=%d; revisar AO->GPIO9 y GND común", raw, mv);
        mq2_desconectado_reportado = true;
    } else if (conectado) {
        mq2_desconectado_reportado = false;
    }

}

static esp_err_t ma40_adc_init(void)
{
#if !MA40_SENSOR_INSTALLED
    ESP_LOGI(TAG, "MA40S4R deshabilitado: sensor no instalado");
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = adc_oneshot_io_to_channel(MA40_ADC_GPIO, &ma40_adc_unit, &ma40_adc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MA40S4R: GPIO%d no es ADC valido (%s)", MA40_ADC_GPIO, esp_err_to_name(ret));
        return ret;
    }

    // En esta placa GPIO9/10/11 están reservados por la microSD y los demás
    // GPIO ADC1 están ocupados por LCD/I2C. GPIO18 es ADC2; ESP-IDF protege
    // las lecturas frente a Wi-Fi. La aplicación usa BLE, no Wi-Fi.

    /* MQ-9 y MA40S4R comparten la misma unidad ADC1. */
    ma40_adc_handle = mq2_adc_handle;
    if (ma40_adc_handle == NULL) {
        const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ma40_adc_unit};
        ret = adc_oneshot_new_unit(&unit_cfg, &ma40_adc_handle);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(ma40_adc_handle, ma40_adc_channel, &chan_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ma40_adc_unit,
        .chan = ma40_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &ma40_cali_handle) == ESP_OK) {
        ma40_adc_calibrado = true;
    }
#endif

    ma40_adc_disponible = true;
    ESP_LOGI(TAG, "MA40S4R amplificado listo: salida ADC=GPIO%d", MA40_ADC_GPIO);
    return ESP_OK;
#endif
}

static void ma40_adc_update(uint32_t now_ms)
{
#if !MA40_SENSOR_INSTALLED
    (void)now_ms;
    return;
#else
    static uint32_t ultimo_log_ma40_ms = 0;
    static int ma40_mv_max_1s = 0;
    bool ma40_evento = false;
    float ma40_umbral_log = 0.0f;
    if (!ma40_adc_disponible || (now_ms - ultimo_ma40_read_ms) < MA40_READ_MS) {
        return;
    }
    ultimo_ma40_read_ms = now_ms;

    int raw = 0;
    int64_t raw_sum = 0;
    const int muestras_adc = 8;
    for (int i = 0; i < muestras_adc; i++) {
        int muestra = 0;
        if (adc_oneshot_read(ma40_adc_handle, ma40_adc_channel, &muestra) != ESP_OK) {
            return;
        }
        raw_sum += muestra;
    }
    raw = (int)(raw_sum / muestras_adc);
    if (raw < 0 || raw > (int)MQ9_ADC_MAX_RAW) {
        return;
    }

    int mv = 0;
    esp_err_t cali_ret = ESP_FAIL;
    if (ma40_adc_calibrado && ma40_cali_handle != NULL) {
        cali_ret = adc_cali_raw_to_voltage(ma40_cali_handle, raw, &mv);
    }
    // La salida del ADC de la ESP32-S3 no puede superar 3.3 V.
    // Si la calibracion devuelve un valor invalido (por ejemplo 4096 mV),
    // usamos una conversion lineal segura como respaldo.
    if (cali_ret != ESP_OK || mv < 0 || mv > 3300) {
        mv = (int)(((int64_t)raw * 3300 + 2047) / 4095);
    }
    if (mv > 3300) {
        mv = 3300;
    }

    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);
    g_state.ma40_raw = (float)raw;
    g_state.ma40_mv = (float)mv;
    g_state.ma40_pct = fminf(100.0f, ((float)raw * 100.0f) / MQ9_ADC_MAX_RAW);
    if (!ma40_baseline_lista) {
        if (ma40_baseline_mv == 0.0f) {
            ma40_baseline_mv = (float)mv;
            ma40_ruido_mv = 20.0f;
        } else {
            float delta = (float)mv - ma40_baseline_mv;
            ma40_baseline_mv += CALIBRATION_ALPHA * delta;
            ma40_ruido_mv = ((1.0f - CALIBRATION_ALPHA) * ma40_ruido_mv) +
                            (CALIBRATION_ALPHA * fabsf(delta));
        }
        if ((now_ms - g_state.inicio_calibracion_ms) >= CALIBRATION_MS) {
            ma40_baseline_lista = true;
        }
        g_state.ma40_detecta_ultrasonido = false;
    } else {
        float margen_ma40 = fmaxf(MA40_MIN_EVENT_MARGIN_MV,
                                  ma40_ruido_mv * MA40_NOISE_MULTIPLIER);
        float umbral_entrada = ma40_baseline_mv + margen_ma40;
        float umbral_salida = ma40_baseline_mv + (margen_ma40 * MA40_RELEASE_RATIO);
        if (!g_state.ma40_detecta_ultrasonido && (float)mv >= umbral_entrada) {
            g_state.ma40_detecta_ultrasonido = true;
            ma40_evento = true;
            ma40_umbral_log = umbral_entrada;
        } else if (g_state.ma40_detecta_ultrasonido && (float)mv <= umbral_salida) {
            g_state.ma40_detecta_ultrasonido = false;
        }
    }
    xSemaphoreGive(telemetry_mutex);

    if (mv > ma40_mv_max_1s) {
        ma40_mv_max_1s = mv;
    }
    if (ma40_evento) {
        ESP_LOGI(TAG, "MA40 ULTRASONIDO: mv=%d base=%.0f umbral=%.0f", mv, ma40_baseline_mv, ma40_umbral_log);
    }
    if ((now_ms - ultimo_log_ma40_ms) >= 1000U) {
        ultimo_log_ma40_ms = now_ms;
        // max_1s: la envolvente analógica dura ~100 ms y se lee cada 50 ms, así que
        // el valor instantáneo se pierde fácil; el máximo del último segundo no.
        ESP_LOGI(TAG, "MA40 GPIO%d: raw=%d mv=%d max_1s=%d base=%.0f%s", MA40_ADC_GPIO, raw, mv,
                 ma40_mv_max_1s, ma40_baseline_mv, (raw >= 4090) ? " SATURADO" : "");
        ma40_mv_max_1s = 0;
    }
#endif
}

/* ============================================================
 * CALIBRACIÓN Y CÁLCULO DE UMBRALES
 * ============================================================ */

static float calcular_umbral_ultrasonido(float baseline)
{
    return fmaxf(MIN_ULTRASONIC_THRESHOLD, baseline * ULTRASONIC_THRESHOLD_MULTIPLIER);
}

static float calcular_umbral_adaptativo(float baseline, float ruido)
{
    float margen_ruido = fmaxf(MIN_ULTRASONIC_MARGIN, ruido * ULTRASONIC_NOISE_SIGMA_MULTIPLIER);
    float umbral_por_ruido = baseline + margen_ruido;
    float umbral_por_ratio = calcular_umbral_ultrasonido(baseline);
    return fmaxf(MIN_ULTRASONIC_THRESHOLD, fmaxf(umbral_por_ruido, umbral_por_ratio));
}

static float calcular_intensidad_pct(float nivel, float baseline, float umbral)
{
    float rango = fmaxf(umbral - baseline, 1.0f);
    float intensidad = ((nivel - baseline) * 100.0f) / rango;
    if (intensidad < 0.0f) {
        return 0.0f;
    }
    if (intensidad > 999.0f) {
        return 999.0f;
    }
    return intensidad;
}

/* ============================================================
 * ESTADO DEL DETECTOR Y MODO DE PRUEBA
 * ============================================================ */

static void reiniciar_estado_deteccion_locked(uint32_t now_ms)
{
    g_state.nivel_audio = 0.0f;
    g_state.nivel_alta_frecuencia = 0.0f;
    g_state.frecuencia_dominante_hz = 0.0f;
    g_state.intensidad_pct = 0.0f;
    g_state.baseline_hf = 0.0f;
    g_state.ruido_hf = 0.0f;
    g_state.umbral_ultrasonido = MIN_ULTRASONIC_THRESHOLD;
    g_state.mq2_gas_detectado = false;
    g_state.ma40_detecta_ultrasonido = false;
    g_state.fft_detecta_ultrasonido = false;
    g_state.fft_sequence = 0;
    ma40_baseline_mv = 0.0f;
    ma40_ruido_mv = 20.0f;
    ma40_baseline_lista = false;
    g_state.baseline_lista = false;
    g_state.inicio_calibracion_ms = now_ms;
    alarma_ultra_reportada = false;
}

static void reiniciar_estado_deteccion(uint32_t now_ms)
{
    if (telemetry_mutex == NULL) {
        reiniciar_estado_deteccion_locked(now_ms);
        return;
    }

    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);
    reiniciar_estado_deteccion_locked(now_ms);
    xSemaphoreGive(telemetry_mutex);
}

static void snapshot_estado(detector_state_t *out_state)
{
    if (telemetry_mutex == NULL) {
        *out_state = g_state;
        return;
    }

    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);
    *out_state = g_state;
    xSemaphoreGive(telemetry_mutex);
}

static void set_mems_capture_status(mems_capture_status_t status)
{
    if (telemetry_mutex == NULL) {
        g_state.mems_capture_status = status;
        return;
    }

    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);
    g_state.mems_capture_status = status;
    xSemaphoreGive(telemetry_mutex);
}

static bool obtener_modo_prueba_activo(void)
{
    bool modo_prueba_activo = false;

    if (telemetry_mutex == NULL) {
        return g_state.modo_prueba_activo;
    }

    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);
    modo_prueba_activo = g_state.modo_prueba_activo;
    xSemaphoreGive(telemetry_mutex);
    return modo_prueba_activo;
}

static void set_modo_prueba(bool activar, uint32_t now_ms)
{
    bool estado_cambio = false;

    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);
    if (g_state.modo_prueba_activo != activar) {
        g_state.modo_prueba_activo = activar;
        estado_cambio = true;
        if (!activar) {
            reiniciar_estado_deteccion_locked(now_ms);
        }
    }
    xSemaphoreGive(telemetry_mutex);

    if (!estado_cambio) {
        return;
    }

    if (activar) {
        inicio_demo_ms = now_ms;
        ESP_LOGW(TAG, "Modo prueba OLED activado. Mantener BOOT %d ms para salir.", DEMO_HOLD_MS);
        return;
    }

    ESP_LOGI(TAG, "Modo prueba OLED desactivado. Recalibrando sensores...");
}

static void actualizar_boton_demo(uint32_t now_ms)
{
    bool boton_apretado = soleac_gpio_boot_button_pressed();

    if (!boton_apretado) {
        boton_demo_presionado_ms = 0;
        boton_demo_consumido = false;
        return;
    }

    if (boton_demo_presionado_ms == 0) {
        boton_demo_presionado_ms = now_ms;
        boton_demo_consumido = false;
        return;
    }

    if (!boton_demo_consumido && (now_ms - boton_demo_presionado_ms) >= DEMO_HOLD_MS) {
        set_modo_prueba(!obtener_modo_prueba_activo(), now_ms);
        boton_demo_consumido = true;
    }
}

static void actualizar_demo(uint32_t now_ms)
{
    if (!obtener_modo_prueba_activo()) {
        return;
    }

    uint32_t elapsed_ms = now_ms - inicio_demo_ms;
    uint32_t fase = (elapsed_ms / 3000U) % 4U;
    float t = (float)elapsed_ms / 1000.0f;
    float onda = 0.5f + (0.5f * sinf(t * 2.0f));

    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);
    g_state.baseline_lista = true;
    g_state.baseline_hf = 80.0f + (10.0f * onda);
    g_state.ruido_hf = 6.0f + (2.0f * onda);
    g_state.umbral_ultrasonido = calcular_umbral_adaptativo(g_state.baseline_hf, g_state.ruido_hf);
    g_state.nivel_audio = 35.0f + (25.0f * onda);
    g_state.frecuencia_dominante_hz = 31500.0f + (4500.0f * sinf(t * 0.7f));

    // Sensores simulados para poder probar la alarma sin conectar una fuga:
    // fases 0-1 = aire comprimido (MQ-9 bajo), fases 2-3 = gas (MQ-9 alto).
    bool simula_gas = (fase >= 2U);
    g_state.ma40_raw = 2800.0f;
    g_state.ma40_mv = 2200.0f;
    g_state.ma40_pct = 68.0f;
    g_state.mq2_conectado = true;
    g_state.mq2_pct = simula_gas ? 65.0f : 5.0f;
    g_state.mq2_gas_detectado = simula_gas;
    g_state.mq2_mv = simula_gas ? 2100.0f : 165.0f;
    g_state.mq2_raw = simula_gas ? 2600.0f : 205.0f;

    switch (fase) {
        case 0:
            g_state.nivel_alta_frecuencia = g_state.baseline_hf + 20.0f + (10.0f * onda);
            break;
        case 1:
            g_state.nivel_alta_frecuencia = g_state.umbral_ultrasonido + 35.0f + (60.0f * onda);
            break;
        case 2:
            g_state.nivel_alta_frecuencia = g_state.baseline_hf + 22.0f + (8.0f * onda);
            break;
        default:
            g_state.nivel_alta_frecuencia = g_state.umbral_ultrasonido + 60.0f + (75.0f * onda);
            break;
    }
    g_state.intensidad_pct = calcular_intensidad_pct(
        g_state.nivel_alta_frecuencia,
        g_state.baseline_hf,
        g_state.umbral_ultrasonido);
    g_state.fft_detecta_ultrasonido =
        g_state.nivel_alta_frecuencia > g_state.umbral_ultrasonido;
    g_state.ma40_detecta_ultrasonido = true;
    xSemaphoreGive(telemetry_mutex);
}

/* ============================================================
 * ALARMAS Y ACTUALIZACIÓN DE TELEMETRÍA
 * ============================================================ */

static void manejar_alarmas(bool alarma_ultra_activa)
{
    if (alarma_ultra_activa) {
        ESP_ERROR_CHECK(soleac_gpio_set_alarm_outputs(true));

        if (!alarma_ultra_reportada) {
            ESP_LOGW(TAG, "Ultrafrecuencia detectada");
        }

        alarma_ultra_reportada = true;
        return;
    }

    alarma_ultra_reportada = false;
    ESP_ERROR_CHECK(soleac_gpio_set_alarm_outputs(false));
}

static void actualizar_telemetria_audio(
    float promedio_audio,
    float nivel_banda_ultrasonica,
    float frecuencia_hz,
    uint32_t now_ms)
{
    bool calibracion_lista = false;
    float umbral_local = MIN_ULTRASONIC_THRESHOLD;
    xSemaphoreTake(telemetry_mutex, portMAX_DELAY);

    if (!g_state.modo_prueba_activo) {
        g_state.fft_sequence++;
        g_state.nivel_audio = (0.85f * g_state.nivel_audio) + (0.15f * promedio_audio);
        g_state.nivel_alta_frecuencia =
            (0.78f * g_state.nivel_alta_frecuencia) + (0.22f * nivel_banda_ultrasonica);
        // El pico espectral es orientativo; presentar frecuencia solo si la
        // energía integrada de banda supera el umbral adaptativo.
        g_state.frecuencia_dominante_hz =
            (g_state.baseline_lista && frecuencia_hz > 0.0f &&
             nivel_banda_ultrasonica >= g_state.umbral_ultrasonido)
                ? frecuencia_hz
                : 0.0f;

        if (!g_state.baseline_lista) {
            if (g_state.baseline_hf == 0.0f) {
                g_state.baseline_hf = g_state.nivel_alta_frecuencia;
                g_state.ruido_hf = MIN_ULTRASONIC_MARGIN;
            } else {
                float delta = g_state.nivel_alta_frecuencia - g_state.baseline_hf;
                g_state.baseline_hf += CALIBRATION_ALPHA * delta;
                g_state.ruido_hf =
                    (1.0f - CALIBRATION_ALPHA) * g_state.ruido_hf + CALIBRATION_ALPHA * fabsf(delta);
            }
            g_state.umbral_ultrasonido = calcular_umbral_adaptativo(g_state.baseline_hf, g_state.ruido_hf);

            if ((now_ms - g_state.inicio_calibracion_ms) >= CALIBRATION_MS) {
                g_state.baseline_lista = true;
                calibracion_lista = true;
            }
        } else if (g_state.nivel_alta_frecuencia < g_state.umbral_ultrasonido) {
            float delta = g_state.nivel_alta_frecuencia - g_state.baseline_hf;
            g_state.baseline_hf += BASELINE_UPDATE_ALPHA * delta;
            g_state.ruido_hf =
                (1.0f - NOISE_UPDATE_ALPHA) * g_state.ruido_hf + NOISE_UPDATE_ALPHA * fabsf(delta);
            g_state.umbral_ultrasonido = calcular_umbral_adaptativo(g_state.baseline_hf, g_state.ruido_hf);
        }

    g_state.intensidad_pct = calcular_intensidad_pct(
            g_state.nivel_alta_frecuencia,
            g_state.baseline_hf,
            g_state.umbral_ultrasonido);
        g_state.fft_detecta_ultrasonido =
            g_state.baseline_lista &&
            g_state.nivel_alta_frecuencia > g_state.umbral_ultrasonido;
        umbral_local = g_state.umbral_ultrasonido;
    }

    xSemaphoreGive(telemetry_mutex);

    if (calibracion_lista) {
        ESP_LOGI(TAG, "Calibracion lista. Umbral ultrasonido: %.2f", umbral_local);
    }
}

/* ============================================================
 * CAPTURA DE AUDIO I2S/PDM
 * ============================================================ */

#if SOLEAC_USE_PDM_RAW
static uint32_t pdm_descartar_durante(uint32_t duration_ms)
{
    static uint16_t descartes[FFT_SIZE];
    uint32_t inicio_ms = esp_log_timestamp();
    uint32_t bloques_completos = 0;

    while ((esp_log_timestamp() - inicio_ms) < duration_ms) {
        size_t bytes = 0;
        esp_err_t ret = i2s_channel_read(
            rx_chan, descartes, sizeof(descartes), &bytes, pdMS_TO_TICKS(10));
        if (ret == ESP_OK && bytes == sizeof(descartes)) {
            bloques_completos++;
        }
    }
    return bloques_completos;
}
#endif

static esp_err_t init_i2s_pdm(bool *pdm_datos_iniciales)
{
    if (pdm_datos_iniciales != NULL) {
        *pdm_datos_iniciales = false;
    }
#if SOLEAC_USE_I2S_PDM_DRIVER
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    // Hacer coincidir cada descriptor DMA con un bloque RAW de FFT (512 bytes)
    // y ampliar el margen ante latencias ocasionales del sistema.
    chan_cfg.dma_frame_num = FFT_SIZE;
    chan_cfg.dma_desc_num = 16;
    esp_err_t channel_ret = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (channel_ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo crear canal I2S PDM (%s)", esp_err_to_name(channel_ret));
        rx_chan = NULL;
        return channel_ret;
    }

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(
#if SOLEAC_USE_PDM_RAW
#if PDM_STAGED_STARTUP_ENABLE
            PDM_STANDARD_DRIVER_RATE_HZ
#else
            PDM_DRIVER_RATE_HZ
#endif
#else
            SAMPLE_RATE_HZ
#endif
        ),
        .slot_cfg =
#if SOLEAC_USE_PDM_RAW
            SOLEAC_PDM_RX_RAW_SLOT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
#else
            SOLEAC_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
#endif
        .gpio_cfg = {
            .clk = pdm_active_clk_pin,
            .din = pdm_active_data_pin,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

#if SOLEAC_USE_PDM_RAW
    // El SPH0641 usa el slot seleccionado por su pin SELECT. La placa SOLEAC
    // debe llevar SELECT a GND para usar el slot izquierdo; si la variante de
    // PCB lo lleva a VDD, cambiar PDM_SENSOR_SLOT_MASK a RIGHT.
    pdm_rx_cfg.slot_cfg.slot_mask = pdm_selected_slot;
#endif

    esp_err_t init_ret = i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg);
    if (init_ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar PDM RX (%s)", esp_err_to_name(init_ret));
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return init_ret;
    }

    // DATA es una salida push-pull del SPH0641 cuando el slot SELECT coincide.
    // No activar el pull-down interno: si SELECT está en el slot opuesto, el
    // micrófono deja DATA en alta impedancia y ese pull-down convierte toda la
    // captura en ceros, ocultando el diagnóstico real.
    esp_err_t pull_ret = gpio_set_pull_mode(pdm_active_data_pin, GPIO_FLOATING);
    if (pull_ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo dejar DATA flotante en GPIO%d (%s)",
                 pdm_active_data_pin, esp_err_to_name(pull_ret));
    }
    esp_err_t enable_ret = i2s_channel_enable(rx_chan);
    if (enable_ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo habilitar canal I2S PDM (%s)", esp_err_to_name(enable_ret));
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return enable_ret;
    }

#if SOLEAC_USE_PDM_RAW
    // El SPH0641 necesita hasta 15 ms para despertar cuando recibe un reloj
    // activo. Drenar y descartar bloques durante ese intervalo evita que el
    // primer probe mida el pin mientras el MEMS todavía está en high-Z.
    uint32_t wakeup_blocks = pdm_descartar_durante(PDM_SENSOR_WAKEUP_MS);
    ESP_LOGI(TAG, "SPH0641 estable tras %u ms; bloques PDM descartados=%u",
             (unsigned)PDM_SENSOR_WAKEUP_MS, (unsigned)wakeup_blocks);

    // SELECT a GND normalmente corresponde a LEFT. Si no hay actividad con el
    // pinout actual, probar también RIGHT para comprobar el mapeo de slots del
    // periférico ESP32-S3; las pruebas anteriores usaban el pinout invertido.
    static uint16_t pdm_slot_test_buffer[FFT_SIZE];
    pdm_selected_slot = I2S_PDM_SLOT_LEFT;
    size_t pdm_test_bytes = 0;
    esp_err_t pdm_test_ret = i2s_channel_read(
        rx_chan, pdm_slot_test_buffer, sizeof(pdm_slot_test_buffer),
        &pdm_test_bytes, pdMS_TO_TICKS(AUDIO_I2S_TIMEOUT_MS));
    float probe_unos_pct = 0.0f;
    float probe_transiciones_pct = 0.0f;
    bool slot_valido = pdm_test_ret == ESP_OK &&
        pdm_test_bytes == sizeof(pdm_slot_test_buffer) &&
        validar_bloque_pdm(pdm_slot_test_buffer, pdm_test_bytes,
                           &probe_unos_pct, &probe_transiciones_pct);
    ESP_LOGI(TAG, "PDM probe DIN=GPIO38 CLK=GPIO39 slot=LEFT(SELECT=GND): %s unos=%.1f%% trans=%.1f%% (%s %u/%u bytes)",
             slot_valido ? "DATOS VALIDOS" : "sin actividad PDM",
             probe_unos_pct,
             probe_transiciones_pct,
             esp_err_to_name(pdm_test_ret), (unsigned)pdm_test_bytes,
             (unsigned)sizeof(pdm_slot_test_buffer));
    if (!slot_valido) {
        esp_err_t slot_ret = i2s_channel_disable(rx_chan);
        if (slot_ret == ESP_OK) {
            pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
            slot_ret = i2s_channel_reconfig_pdm_rx_slot(rx_chan, &pdm_rx_cfg.slot_cfg);
            if (slot_ret == ESP_OK) {
                slot_ret = i2s_channel_enable(rx_chan);
            } else {
                // Si no se pudo aplicar RIGHT, volver a habilitar el LEFT
                // original para dejar la captura en un estado conocido.
                pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
                esp_err_t restore_ret = i2s_channel_enable(rx_chan);
                if (restore_ret != ESP_OK) {
                    ESP_LOGE(TAG, "No se pudo reactivar I2S tras fallar prueba RIGHT (%s)",
                             esp_err_to_name(restore_ret));
                }
            }
        }

        if (slot_ret == ESP_OK) {
            pdm_selected_slot = I2S_PDM_SLOT_RIGHT;
            uint32_t right_warmup_blocks = pdm_descartar_durante(PDM_SENSOR_WAKEUP_MS);
            size_t right_bytes = 0;
            esp_err_t right_read_ret = i2s_channel_read(
                rx_chan, pdm_slot_test_buffer, sizeof(pdm_slot_test_buffer),
                &right_bytes, pdMS_TO_TICKS(AUDIO_I2S_TIMEOUT_MS));
            float right_unos_pct = 0.0f;
            float right_transiciones_pct = 0.0f;
            bool right_valido = right_read_ret == ESP_OK &&
                right_bytes == sizeof(pdm_slot_test_buffer) &&
                validar_bloque_pdm(pdm_slot_test_buffer, right_bytes,
                                   &right_unos_pct, &right_transiciones_pct);
            ESP_LOGI(TAG, "PDM probe DIN=GPIO38 CLK=GPIO39 slot=RIGHT: %s unos=%.1f%% trans=%.1f%% (%s %u/%u bytes); warmup=%u",
                     right_valido ? "DATOS VALIDOS" : "sin actividad PDM",
                     right_unos_pct, right_transiciones_pct,
                     esp_err_to_name(right_read_ret), (unsigned)right_bytes,
                     (unsigned)sizeof(pdm_slot_test_buffer), (unsigned)right_warmup_blocks);
            slot_valido = right_valido;

            if (!right_valido) {
                esp_err_t restore_ret = i2s_channel_disable(rx_chan);
                if (restore_ret == ESP_OK) {
                    pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
                    restore_ret = i2s_channel_reconfig_pdm_rx_slot(rx_chan, &pdm_rx_cfg.slot_cfg);
                    if (restore_ret == ESP_OK) {
                        restore_ret = i2s_channel_enable(rx_chan);
                    } else {
                        esp_err_t enable_ret_restore = i2s_channel_enable(rx_chan);
                        if (enable_ret_restore != ESP_OK) {
                            ESP_LOGE(TAG, "No se pudo reactivar I2S tras restaurar LEFT (%s)",
                                     esp_err_to_name(enable_ret_restore));
                        }
                    }
                }
                if (restore_ret == ESP_OK) {
                    pdm_selected_slot = I2S_PDM_SLOT_LEFT;
                    (void)pdm_descartar_durante(PDM_SENSOR_WAKEUP_MS);
                } else {
                    ESP_LOGE(TAG, "No se pudo restaurar slot LEFT (%s)",
                             esp_err_to_name(restore_ret));
                }
            }
        } else {
            ESP_LOGW(TAG, "No se pudo probar slot RIGHT (%s); se conserva LEFT",
                     esp_err_to_name(slot_ret));
            pdm_selected_slot = I2S_PDM_SLOT_LEFT;
        }

        if (!slot_valido) {
            ESP_LOGE(TAG, "GPIO38 sigue sin PDM valido con CLK en GPIO39, ni LEFT ni RIGHT");
        }
    }
    if (pdm_datos_iniciales != NULL) {
        *pdm_datos_iniciales = slot_valido;
    }
    set_mems_capture_status(slot_valido ? MEMS_CAPTURE_OK : MEMS_CAPTURE_NO_DATA);
#endif

#if SOLEAC_USE_PDM_RAW && PDM_STAGED_STARTUP_ENABLE
    // El SPH0641LU4H-1 tiene una restricción de arranque: no debe recibir
    // directamente un reloj ultrasónico. Se lo despierta en modo estándar,
    // se detiene el canal y recién entonces se conmuta a 3.072 MHz.
    ESP_LOGI(TAG,
             "SPH0641: arranque estandar clk=%d Hz durante %d ms; luego modo ultrasonico clk=%d Hz",
             PDM_STANDARD_SENSOR_CLOCK_HZ,
             PDM_STANDARD_STARTUP_DELAY_MS,
             PDM_SENSOR_CLOCK_HZ);
    vTaskDelay(pdMS_TO_TICKS(PDM_STANDARD_STARTUP_DELAY_MS));

    esp_err_t disable_ret = i2s_channel_disable(rx_chan);
    if (disable_ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo detener PDM para cambiar a modo ultrasonico (%s)",
                 esp_err_to_name(disable_ret));
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return disable_ret;
    }

    i2s_pdm_rx_clk_config_t ultrasonic_clk_cfg =
        I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_DRIVER_RATE_HZ);
    esp_err_t reconfig_ret = i2s_channel_reconfig_pdm_rx_clock(rx_chan, &ultrasonic_clk_cfg);
    if (reconfig_ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar reloj PDM ultrasonico de %d Hz (%s)",
                 PDM_SENSOR_CLOCK_HZ, esp_err_to_name(reconfig_ret));
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return reconfig_ret;
    }

    enable_ret = i2s_channel_enable(rx_chan);
    if (enable_ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo reactivar PDM en modo ultrasonico (%s)",
                 esp_err_to_name(enable_ret));
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return enable_ret;
    }

    // Espera al cambio de modo y descarta el primer bloque, que puede contener
    // bits capturados durante la transición entre ambos relojes.
    vTaskDelay(pdMS_TO_TICKS(PDM_ULTRASONIC_SETTLE_DELAY_MS));
    uint16_t pdm_warmup_buffer[FFT_SIZE];
    size_t pdm_warmup_bytes = 0;
    esp_err_t warmup_ret = i2s_channel_read(
        rx_chan,
        pdm_warmup_buffer,
        sizeof(pdm_warmup_buffer),
        &pdm_warmup_bytes,
        pdMS_TO_TICKS(AUDIO_I2S_TIMEOUT_MS));
    if (warmup_ret != ESP_OK || pdm_warmup_bytes != sizeof(pdm_warmup_buffer)) {
        ESP_LOGW(TAG, "PDM cambio a ultrasonico sin bloque de descarte completo: %s bytes=%u/%u",
                 esp_err_to_name(warmup_ret),
                 (unsigned)pdm_warmup_bytes,
                 (unsigned)sizeof(pdm_warmup_buffer));
    }
#endif
    ESP_LOGI(
        TAG,
#if SOLEAC_USE_PDM_RAW
        "SPH0641LU4H-1 PDM RAW: clk_pin=%d Hz, pcm=%d Hz, decim=%d, slot=%s, DIN=%d CLK=%d",
        PDM_SENSOR_CLOCK_HZ,
        SAMPLE_RATE_HZ,
        PDM_RAW_DECIMATION,
        (pdm_selected_slot == I2S_PDM_SLOT_LEFT) ? "LEFT" : "RIGHT",
#else
        "SPH0641LU4H-1 PDM RX: clk=%d Hz, pcm=%d Hz, DIN=%d CLK=%d",
        PDM_SENSOR_CLOCK_HZ,
        SAMPLE_RATE_HZ,
#endif
        pdm_active_data_pin,
        pdm_active_clk_pin);
#else
    i2s_config_t i2s_config = {0};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    i2s_config.sample_rate = SAMPLE_RATE_HZ;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
#if defined(I2S_COMM_FORMAT_STAND_I2S)
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
#else
    i2s_config.communication_format = I2S_COMM_FORMAT_I2S;
#endif
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = DMA_BUFFER_COUNT;
    i2s_config.dma_buf_len = BUFFER_SAMPLES;
    i2s_config.use_apll = false;

    i2s_pin_config_t pin_config = {0};
    pin_config.bck_io_num = I2S_PIN_NO_CHANGE;
    pin_config.ws_io_num = I2S_CLK_PIN;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;
    pin_config.data_in_num = I2S_DATA_PIN;

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));
    ESP_ERROR_CHECK(gpio_set_pull_mode(I2S_DATA_PIN, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_PORT));
    ESP_LOGI(
        TAG,
        "SPH0641LU4H-1 PDM RX legacy: clk=%d Hz aprox, pcm=%d Hz, 16-bit mono, DIN=%d CLK=%d",
        PDM_SENSOR_CLOCK_HZ,
        SAMPLE_RATE_HZ,
        I2S_DATA_PIN,
        I2S_CLK_PIN);
#endif
    return ESP_OK;
}

static esp_err_t leer_muestras_audio(void *buffer, size_t bytes_to_read, size_t *bytes_read)
{
#if SOLEAC_USE_I2S_PDM_DRIVER
    return i2s_channel_read(
        rx_chan,
        buffer,
        bytes_to_read,
        bytes_read,
        pdMS_TO_TICKS(AUDIO_I2S_TIMEOUT_MS));
#else
    return i2s_read(
        I2S_PORT,
        buffer,
        bytes_to_read,
        bytes_read,
        pdMS_TO_TICKS(AUDIO_I2S_TIMEOUT_MS));
#endif
}

#if SOLEAC_USE_PDM_RAW
/*
 * Valida el bloque antes de decimarlo. El porcentaje de unos revela una
 * línea fijada a GND/VCC y las transiciones revelan una línea desconectada
 * que quedó estática. Las métricas se devuelven al probe de arranque; la
 * tarea de captura no imprime diagnósticos periódicos mientras consume DMA.
 */
static bool validar_bloque_pdm(
    const uint16_t *pdm,
    size_t bytes,
    float *unos_pct_out,
    float *transiciones_pct_out)
{
    const size_t words = bytes / sizeof(*pdm);
    const size_t total_bits = words * 16U;
    size_t unos = 0;
    size_t transiciones = 0;
    int bit_anterior = -1;

    for (size_t i = 0; i < words; i++) {
        uint16_t palabra = pdm[i];
        for (int j = 0; j < 16; j++) {
            int bit = (palabra >> j) & 1U;
            unos += (size_t)bit;
            if (bit_anterior >= 0 && bit != bit_anterior) {
                transiciones++;
            }
            bit_anterior = bit;
        }
    }

    float unos_pct = total_bits > 0U ? ((float)unos * 100.0f) / (float)total_bits : 0.0f;
    float transiciones_pct = total_bits > 1U
                                 ? ((float)transiciones * 100.0f) / (float)(total_bits - 1U)
                                 : 0.0f;
    if (unos_pct_out != NULL) {
        *unos_pct_out = unos_pct;
    }
    if (transiciones_pct_out != NULL) {
        *transiciones_pct_out = transiciones_pct;
    }
    bool valido = total_bits > 1U &&
                  unos_pct >= PDM_MIN_ONES_PERCENT &&
                  unos_pct <= PDM_MAX_ONES_PERCENT &&
                  transiciones_pct >= PDM_MIN_TRANSITIONS_PERCENT;

    return valido;
}
#endif

/* ============================================================
 * ANÁLISIS FFT
 *
 * Estas funciones no deben modificar g_state. Reciben un bloque de
 * muestras y devuelven únicamente la magnitud y frecuencia detectadas.
 * ============================================================ */

static float interpolar_pico_hz(int bin, float mag_prev, float mag_peak, float mag_next)
{
    float denom = (mag_prev - (2.0f * mag_peak) + mag_next);
    float offset = 0.0f;

    if (fabsf(denom) > 1e-9f) {
        offset = 0.5f * (mag_prev - mag_next) / denom;
        if (offset < -0.5f) {
            offset = -0.5f;
        } else if (offset > 0.5f) {
            offset = 0.5f;
        }
    }

    return ((float)bin + offset) * FFT_BIN_HZ;
}

static float estimar_nivel_banda_ultrasonica_fft(
    const int16_t *buffer, int muestras, float dc, float *frecuencia_hz)
{
#if SOLEAC_USE_ESP_DSP
    int first_bin = SEARCH_BIN_START;
    int last_bin = SEARCH_BIN_END;
    int peak_bin = first_bin;
    float peak_mag_sq = 0.0f;
    float band_power = 0.0f;
    float window_power = 0.0f;
    float prev_mag = 0.0f;
    float next_mag = 0.0f;

    if (first_bin < 1) {
        first_bin = 1;
    }
    if (last_bin > ((FFT_SIZE / 2) - 2)) {
        last_bin = (FFT_SIZE / 2) - 2;
    }

    for (int i = 0; i < muestras; i++) {
        float window = fft_window[i];
        float muestra_centrada = ((float)buffer[i] - dc) * window;
        fft_input_complex[i * 2] = muestra_centrada;
        fft_input_complex[(i * 2) + 1] = 0.0f;
        window_power += window * window;
    }

    // Usar la implementación ANSI para evitar la rutina AES3 optimizada,
    // que en esta ESP32-S3 queda ejecutándose dentro de la FFT y dispara
    // el task watchdog antes de que IDLE1 pueda ejecutarse.
    esp_err_t fft_ret = dsps_fft2r_fc32_ansi(fft_input_complex, FFT_SIZE);
    if (fft_ret != ESP_OK) {
        *frecuencia_hz = 0.0f;
        return 0.0f;
    }
    dsps_bit_rev_fc32(fft_input_complex, FFT_SIZE);
    dsps_cplx2reC_fc32(fft_input_complex, FFT_SIZE);

    for (int bin = first_bin; bin <= last_bin; bin++) {
        float real = fft_input_complex[bin * 2];
        float imag = fft_input_complex[(bin * 2) + 1];
        float mag_sq = (real * real) + (imag * imag);
        band_power += mag_sq;
        if (mag_sq > peak_mag_sq) {
            peak_mag_sq = mag_sq;
            peak_bin = bin;
        }
    }

    // Las fugas de aire suelen generar ruido ultrasónico de banda ancha, no
    // un tono estable. Integrar potencia en toda la banda 20-55 kHz permite
    // detectar esa energía; el máximo espectral se conserva solo para dar una
    // frecuencia orientativa. La normalización por ventana entrega RMS.
    float band_rms = (window_power > 0.0f)
                         ? sqrtf((2.0f * band_power) /
                                 ((float)FFT_SIZE * window_power))
                         : 0.0f;
    prev_mag = sqrtf(
        (fft_input_complex[(peak_bin - 1) * 2] * fft_input_complex[(peak_bin - 1) * 2]) +
        (fft_input_complex[((peak_bin - 1) * 2) + 1] * fft_input_complex[((peak_bin - 1) * 2) + 1])) /
        (float)FFT_SIZE;
    next_mag = sqrtf(
        (fft_input_complex[(peak_bin + 1) * 2] * fft_input_complex[(peak_bin + 1) * 2]) +
        (fft_input_complex[((peak_bin + 1) * 2) + 1] * fft_input_complex[((peak_bin + 1) * 2) + 1])) /
        (float)FFT_SIZE;

    float peak_mag = sqrtf(peak_mag_sq) / (float)FFT_SIZE;
    *frecuencia_hz = interpolar_pico_hz(peak_bin, prev_mag, peak_mag, next_mag);
    return band_rms;
#else
    (void)buffer;
    (void)muestras;
    (void)dc;
    *frecuencia_hz = 0.0f;
    return 0.0f;
#endif
}

/* ============================================================
 * TAREA PRINCIPAL DE PROCESAMIENTO DE AUDIO
 * ============================================================ */

static void audio_processing_task(void *pvParameters)
{
    bool estaba_en_demo = false;
#if !SOLEAC_USE_ESP_DSP
    bool muestra_anterior_valida = false;
    int32_t muestra_anterior = 0;
#endif
    // Estos buffers no deben vivir en la pila: con PDM RAW ocupan más de 4 kB
    // juntos y provocan overflow en la tarea de audio.
    static int16_t buffer[BUFFER_SAMPLES];
#if SOLEAC_USE_PDM_RAW
    static uint16_t pdm_raw_buffer[FFT_SIZE];
    bool mems_estado_local = false;
    uint32_t pdm_invalido_desde_ms = 0;
#endif

    (void)pvParameters;

    // La captura PDM se inicializa dentro de esta tarea para que un fallo del
    // driver o del cableado no bloquee el arranque de la pantalla y del resto
    // de la telemetria.
    bool pdm_datos_iniciales = false;
    esp_err_t i2s_ret = init_i2s_pdm(&pdm_datos_iniciales);
    if (i2s_ret != ESP_OK) {
        set_mems_capture_status(MEMS_CAPTURE_NO_DATA);
        ESP_LOGE(TAG, "Audio MEMS deshabilitado: %s", esp_err_to_name(i2s_ret));
        vTaskDelete(NULL);
        return;
    }

#if SOLEAC_USE_PDM_RAW
    // El probe de arranque solo leyó un bloque; exigir una secuencia continua
    // en la captura normal antes de declarar que el MEMS está operativo.
    mems_estado_local = false;
    uint8_t pdm_bloques_validos_consecutivos = 0;
    set_mems_capture_status(MEMS_CAPTURE_STARTING);
#endif

    ESP_LOGI(TAG,
             "MEMS listo. Mantener el emisor de 40 kHz APAGADO durante %d ms para calibrar y encenderlo despues.",
             CALIBRATION_MS);

#if SOLEAC_USE_ESP_DSP
    esp_err_t fft_status = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (fft_status != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar esp-dsp FFT: %s", esp_err_to_name(fft_status));
        vTaskDelete(NULL);
        return;
    }

    dsps_wind_hann_f32(fft_window, FFT_SIZE);
    ESP_LOGI(TAG,
             "SPH0641 PDM%s + FFT activa en Core %d: pdm_clk=%d Hz, fs=%d Hz, N=%d, %.1f Hz/bin, busqueda %.1f-%.1f kHz",
#if SOLEAC_USE_PDM_RAW
             " RAW",
#else
             "",
#endif
             xPortGetCoreID(),
             PDM_SENSOR_CLOCK_HZ,
             SAMPLE_RATE_HZ,
             FFT_SIZE,
             FFT_BIN_HZ,
             ULTRASONIC_SEARCH_FREQ_MIN_HZ / 1000.0f,
             ULTRASONIC_SEARCH_FREQ_MAX_HZ / 1000.0f);
#else
    ESP_LOGW(TAG, "esp-dsp no disponible. Se usa el detector HF simplificado; no habra frecuencia FFT.");
    ESP_LOGI(TAG, "Tarea de audio iniciada en Core %d", xPortGetCoreID());
#endif

    while (1) {
        size_t bytes_read = 0;
        uint32_t now_ms = esp_log_timestamp();
        static uint32_t lecturas_i2s_fallidas = 0;

        mq2_adc_update(now_ms);
        ma40_adc_update(now_ms);

        if (obtener_modo_prueba_activo()) {
            estaba_en_demo = true;
#if !SOLEAC_USE_ESP_DSP
            muestra_anterior_valida = false;
#endif
            vTaskDelay(pdMS_TO_TICKS(AUDIO_IDLE_DELAY_MS));
            continue;
        }

        if (estaba_en_demo) {
#if !SOLEAC_USE_ESP_DSP
            muestra_anterior_valida = false;
#endif
            estaba_en_demo = false;
        }

#if SOLEAC_USE_PDM_RAW
        esp_err_t status = leer_muestras_audio(pdm_raw_buffer, sizeof(pdm_raw_buffer), &bytes_read);
        // Validar inmediatamente el bloque leído: no enviar una entrada PDM
        // fija o flotante al decimador ni a la FFT.
        bool pdm_bloque_valido =
            status == ESP_OK && bytes_read == sizeof(pdm_raw_buffer) &&
            validar_bloque_pdm(pdm_raw_buffer, bytes_read, NULL, NULL);
        if (pdm_bloque_valido) {
            if (pdm_bloques_validos_consecutivos < PDM_VALID_BLOCKS_TO_CONFIRM) {
                pdm_bloques_validos_consecutivos++;
            }
            if (pdm_bloques_validos_consecutivos >= PDM_VALID_BLOCKS_TO_CONFIRM) {
                pdm_invalido_desde_ms = 0;
            }
            if (!mems_estado_local &&
                pdm_bloques_validos_consecutivos >= PDM_VALID_BLOCKS_TO_CONFIRM) {
                mems_estado_local = true;
                set_mems_capture_status(MEMS_CAPTURE_OK);
            }
        } else {
            pdm_bloques_validos_consecutivos = 0;
            if (pdm_invalido_desde_ms == 0) {
                pdm_invalido_desde_ms = now_ms;
            } else if ((now_ms - pdm_invalido_desde_ms) >= MEMS_NO_DATA_TIMEOUT_MS) {
                mems_estado_local = false;
                set_mems_capture_status(MEMS_CAPTURE_NO_DATA);
            }
        }
#else
        esp_err_t status = leer_muestras_audio(buffer, sizeof(buffer), &bytes_read);
#endif
#if SOLEAC_USE_PDM_RAW
        size_t bytes_esperados = sizeof(pdm_raw_buffer);
#else
        size_t bytes_esperados = sizeof(buffer);
#endif
        if (status != ESP_OK || bytes_read < bytes_esperados) {
            lecturas_i2s_fallidas++;
            // No saturar el monitor serie: informar una vez cada 50 fallos.
            if ((lecturas_i2s_fallidas % 50U) == 1U) {
                ESP_LOGW(TAG,
                         "MEMS/I2S sin bloque completo: status=%s bytes=%u/%u fallos=%u",
                         esp_err_to_name(status),
                         (unsigned)bytes_read,
                         (unsigned)bytes_esperados,
                         (unsigned)lecturas_i2s_fallidas);
            }
            // El timeout de i2s_channel_read ya evita el bucle ocupado.
            taskYIELD();
            continue;
        }

#if SOLEAC_USE_PDM_RAW
        if (!pdm_bloque_valido) {
            taskYIELD();
            continue;
        }
        if (!mems_estado_local ||
            pdm_bloques_validos_consecutivos < PDM_VALID_BLOCKS_TO_CONFIRM) {
            taskYIELD();
            continue;
        }
#endif

        if (lecturas_i2s_fallidas != 0U) {
            ESP_LOGI(TAG, "MEMS/I2S recuperado despues de %u fallos",
                     (unsigned)lecturas_i2s_fallidas);
            lecturas_i2s_fallidas = 0;
        }

        int muestras = 0;
#if SOLEAC_USE_PDM_RAW
        // Cada palabra de 16 bits contiene 16 muestras PDM. El popcount
        // implementa el mismo promedio que antes, pero sin cuatro delays de
        // un tick dentro de cada bloque. Con CONFIG_FREERTOS_HZ=100, cada
        // vTaskDelay(1) podía dormir 10 ms y desbordar el DMA.
        for (int i = 0; i < FFT_SIZE; i++) {
            int32_t unos = __builtin_popcount((unsigned)pdm_raw_buffer[i]);
            int32_t suma_pdm = (2 * unos) - PDM_RAW_DECIMATION;
            buffer[i] = (int16_t)(suma_pdm * (2048 / PDM_RAW_DECIMATION));
        }
        muestras = FFT_SIZE;
#else
        muestras = (int)(bytes_read / sizeof(int16_t));
#endif
        if (muestras != FFT_SIZE) {
            taskYIELD();
            continue;
        }

        int64_t suma = 0;
        for (int i = 0; i < muestras; i++) {
            suma += buffer[i];
        }
        float dc = (float)suma / (float)muestras;

        float energia_audio = 0.0f;
#if !SOLEAC_USE_ESP_DSP
        float energia_alta = 0.0f;
        int diferencias = 0;
        int32_t prev = muestra_anterior;
        bool prev_valida = muestra_anterior_valida;
#endif

        for (int i = 0; i < muestras; i++) {
            float muestra_centrada = (float)buffer[i] - dc;
            energia_audio += fabsf(muestra_centrada);

#if !SOLEAC_USE_ESP_DSP
            if (prev_valida) {
                energia_alta += (float)abs((int)muestra_centrada - prev);
                diferencias++;
            }

            prev = (int32_t)muestra_centrada;
            prev_valida = true;
#endif
        }

#if !SOLEAC_USE_ESP_DSP
        muestra_anterior = prev;
        muestra_anterior_valida = prev_valida;
#endif

        float promedio_audio = energia_audio / (float)muestras;
        float promedio_alta = 0.0f;
        float frecuencia_hz = 0.0f;

#if SOLEAC_USE_ESP_DSP
        promedio_alta = estimar_nivel_banda_ultrasonica_fft(
            buffer, muestras, dc, &frecuencia_hz);
#else
        promedio_alta = (diferencias > 0) ? (energia_alta / (float)diferencias) : 0.0f;
#endif

        actualizar_telemetria_audio(promedio_audio, promedio_alta, frecuencia_hz, now_ms);
        // La próxima lectura I2S bloquea hasta que llegue el siguiente bloque;
        // no dormir aquí porque perdería varios bloques con tick de 100 Hz.
        taskYIELD();
    }
}

/* ============================================================
 * API PÚBLICA
 * ============================================================ */

esp_err_t fft_processing_init(uint32_t now_ms)
{
    soleac_gpio_init();

    telemetry_mutex = xSemaphoreCreateMutex();
    if (telemetry_mutex == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el mutex de telemetria.");
        return ESP_ERR_NO_MEM;
    }

    reiniciar_estado_deteccion(now_ms);
    esp_err_t mq2_ret = mq2_adc_init();
    if (mq2_ret != ESP_OK) {
        ESP_LOGW(TAG, "MQ-9 deshabilitado (%s). Continuando solo con ultrasonido.", esp_err_to_name(mq2_ret));
    }
    esp_err_t ma40_ret = ma40_adc_init();
    if (ma40_ret != ESP_OK) {
        ESP_LOGW(TAG, "MA40S4R deshabilitado (%s)", esp_err_to_name(ma40_ret));
    }
    // El driver PDM se inicializa dentro de fft_processing_task. Así un fallo
    // del MEMS no evita que LCD, MQ-9 y MA40 puedan seguir funcionando.
    return ESP_OK;
}

void fft_processing_task(void *pvParameters)
{
    audio_processing_task(pvParameters);
}

void fft_processing_snapshot(detector_state_t *out_state)
{
    snapshot_estado(out_state);
}

bool fft_processing_get_test_mode(void)
{
    return obtener_modo_prueba_activo();
}

void fft_processing_set_test_mode(bool activar, uint32_t now_ms)
{
    set_modo_prueba(activar, now_ms);
}

void fft_processing_update_boot_button(uint32_t now_ms)
{
    actualizar_boton_demo(now_ms);
}

void fft_processing_update_demo(uint32_t now_ms)
{
    actualizar_demo(now_ms);
}

void fft_processing_handle_alarms(bool alarma_ultra_activa)
{
    manejar_alarmas(alarma_ultra_activa);
}
