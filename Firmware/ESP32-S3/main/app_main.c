#include <stdbool.h>
#include <stdint.h>
#include "ble_comm.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fft_processing.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_control.h"
#include "sd_logger.h"
#include "nvs_flash.h"

#define AUDIO_TASK_STACK_SIZE 6144
#define AUDIO_TASK_PRIORITY 10
#define FUGA_VENTANA_EVALUACIONES 8
#define FUGA_MIN_SOSPECHOSAS 3
#define FUGA_LIBERACION_NORMALES 4
// Durante la puesta en marcha se prueba únicamente el MEMS/FFT. Activar en 1
// cuando el MA40S4R esté instalado y calibrado en la placa.
#define FUGA_REQUIERE_MA40 0
// La LCD se mantiene activa para mostrar los datos de la FFT durante la
// prueba del MEMS.
#define LCD_INIT_ENABLE 1

static const char *TAG = "DETECTOR_FUGA";

static bool actualizar_deteccion_persistente(const detector_state_t *state)
{
    static uint8_t historial = 0;
    static uint8_t evaluaciones = 0;
    static uint8_t normales_consecutivas = 0;
    static bool fuga_confirmada = false;
    static uint32_t ultima_fft_sequence = 0;

    // No acumular positivos de FFT cuando el flujo PDM está fijado o no válido.
    // En ese caso, borrar también una alarma retenida hasta que la captura se
    // recupere y vuelva a reunir una ventana completa de evaluaciones.
    if (!state->baseline_lista || state->mems_capture_status != MEMS_CAPTURE_OK) {
        historial = 0;
        evaluaciones = 0;
        normales_consecutivas = 0;
        fuga_confirmada = false;
        ultima_fft_sequence = state->fft_sequence;
        return false;
    }

    // app_main corre cada 20 ms y puede observar varias veces el mismo estado.
    // Solo contar una evaluación cuando el procesador publicó una FFT nueva.
    if (state->fft_sequence == ultima_fft_sequence) {
        return fuga_confirmada;
    }
    ultima_fft_sequence = state->fft_sequence;

    // La FFT es suficiente durante la prueba del MEMS. El MA40S4R puede
    // volver a funcionar como segundo canal activando FUGA_REQUIERE_MA40.
    bool sospechosa = state->fft_detecta_ultrasonido;
#if FUGA_REQUIERE_MA40
    sospechosa = sospechosa && state->ma40_detecta_ultrasonido;
#endif
    historial = (uint8_t)((historial << 1) | (sospechosa ? 1U : 0U));
    if (evaluaciones < FUGA_VENTANA_EVALUACIONES) {
        evaluaciones++;
    }

    uint8_t sospechosas = 0;
    uint8_t muestra = historial;
    while (muestra != 0U) {
        sospechosas += (uint8_t)(muestra & 1U);
        muestra >>= 1;
    }

    if (!fuga_confirmada && evaluaciones >= FUGA_VENTANA_EVALUACIONES &&
        sospechosas >= FUGA_MIN_SOSPECHOSAS) {
        fuga_confirmada = true;
        normales_consecutivas = 0;
        ESP_LOGW(TAG, "FUGA CONFIRMADA: %u de %u FFT sospechosas, frecuencia=%.0f Hz HF=%.1f",
                 sospechosas, FUGA_VENTANA_EVALUACIONES,
                  state->frecuencia_dominante_hz, state->nivel_alta_frecuencia);
    } else if (fuga_confirmada) {
        if (sospechosa) {
            normales_consecutivas = 0;
        } else if (normales_consecutivas < FUGA_LIBERACION_NORMALES) {
            normales_consecutivas++;
        }

        if (normales_consecutivas >= FUGA_LIBERACION_NORMALES) {
            fuga_confirmada = false;
            historial = 0;
            evaluaciones = 0;
            normales_consecutivas = 0;
            ESP_LOGI(TAG, "Fuga liberada: %u evaluaciones normales", FUGA_LIBERACION_NORMALES);
        }
    }

    return fuga_confirmada;
}

void app_main(void)
{
    uint32_t now_ms = esp_log_timestamp();
    detector_state_t state = {0};
    BaseType_t task_ret;

    ESP_LOGI(TAG, "Iniciando SOLEAC - Detector de Fugas Ultrasonico");
    ESP_LOGI(TAG, "Hardware: ESP32-S3 + Waveshare LCD 3.5\" AXS15231B");

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requiere limpieza (%s). Borrando particion...", esp_err_to_name(nvs_ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS no disponible: %s", esp_err_to_name(nvs_ret));
        return;
    }

    esp_err_t ble_ret = ble_comm_init();
    if (ble_ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE no disponible (%s). Continuando sin BLE.", esp_err_to_name(ble_ret));
    }

    esp_err_t fft_ret = fft_processing_init(now_ms);
    if (fft_ret != ESP_OK) {
        ESP_LOGE(TAG, "FFT/MEMS no disponible (%s). Continuando con LCD y BLE.", esp_err_to_name(fft_ret));
    }

    if (fft_ret == ESP_OK) {
        task_ret = xTaskCreatePinnedToCore(
            fft_processing_task,
            "fft_processing_task",
            AUDIO_TASK_STACK_SIZE,
            NULL,
            AUDIO_TASK_PRIORITY,
            NULL,
            1);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "No se pudo crear la tarea de audio.");
        }
    }

    esp_err_t sd_ret = sd_logger_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "Registro en microSD no disponible: %s", esp_err_to_name(sd_ret));
    }

#if LCD_INIT_ENABLE
    // El MEMS debe empezar a capturar aunque el I2C del expansor o la LCD
    // tarde demasiado. Así un problema de pantalla no oculta el diagnóstico
    // PDM/FFT.
    esp_err_t lcd_ret = lcd_control_init();
    if (lcd_ret != ESP_OK) {
        ESP_LOGW(TAG, "LCD no disponible (%s). Continuando sin pantalla.", esp_err_to_name(lcd_ret));
    }
#endif

    while (1) {
        static uint32_t ultimo_estado_log_ms = 0;
        now_ms = esp_log_timestamp();

        if (lcd_control_touch_demo_requested(now_ms)) {
            fft_processing_set_test_mode(true, now_ms);
        }

        fft_processing_update_boot_button(now_ms);

        bool activar_demo_ble;
        if (fft_ret == ESP_OK && ble_comm_take_demo_command(&activar_demo_ble)) {
            fft_processing_set_test_mode(activar_demo_ble, now_ms);
        }

        if (fft_processing_get_test_mode()) {
            fft_processing_update_demo(now_ms);
        }

        fft_processing_snapshot(&state);
        if ((now_ms - ultimo_estado_log_ms) >= 5000U) {
            const char *mems_status = "iniciando";
            if (state.mems_capture_status == MEMS_CAPTURE_NO_DATA) {
                mems_status = "SIN DATOS";
            } else if (state.mems_capture_status == MEMS_CAPTURE_OK) {
                mems_status = "OK";
            }
            ESP_LOGI(TAG,
                     "ESTADO: MEMS=%s calibracion=%s FREQ=%.0fHz HF=%.1f umbral=%.1f DETECT=%s MQ2=%s %.1f%% MA40=%d mV",
                     mems_status,
                     state.baseline_lista ? "OK" : "en curso",
                     state.frecuencia_dominante_hz,
                     state.nivel_alta_frecuencia,
                     state.umbral_ultrasonido,
                     state.fft_detecta_ultrasonido ? "SI" : "NO",
                     state.mq2_conectado ? "conectado" : "desconectado",
                     state.mq2_pct,
                     (int)state.ma40_mv);
            ultimo_estado_log_ms = now_ms;
        }
        bool alarma_ultra_activa = state.modo_prueba_activo || actualizar_deteccion_persistente(&state);

        static uint32_t ultimo_sd_log_ms = 0;
        if ((now_ms - ultimo_sd_log_ms) >= 1000U) {
            ultimo_sd_log_ms = now_ms;
            sd_logger_enqueue(now_ms, &state, alarma_ultra_activa);
        }

        fft_processing_handle_alarms(alarma_ultra_activa);
        lcd_control_render(now_ms, &state, alarma_ultra_activa);
        ble_comm_notify_telemetria(now_ms, &state, alarma_ultra_activa);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
