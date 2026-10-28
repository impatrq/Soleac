#ifndef FFT_PROCESSING_H
#define FFT_PROCESSING_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    MEMS_CAPTURE_STARTING = 0,
    MEMS_CAPTURE_OK,
    MEMS_CAPTURE_NO_DATA,
} mems_capture_status_t;

typedef struct {
    float nivel_audio;
    float nivel_alta_frecuencia;
    float frecuencia_dominante_hz;
    float intensidad_pct;
    float mq2_raw;
    float mq2_mv;
    float mq2_pct;
    bool mq2_conectado;
    bool mq2_gas_detectado;
    float ma40_raw;
    float ma40_mv;
    float ma40_pct;
    bool ma40_detecta_ultrasonido;
    bool fft_detecta_ultrasonido;
    float baseline_hf;
    float ruido_hf;
    float umbral_ultrasonido;
    bool baseline_lista;
    mems_capture_status_t mems_capture_status;
    uint32_t fft_sequence;
    bool modo_prueba_activo;
    uint32_t inicio_calibracion_ms;
} detector_state_t;

esp_err_t fft_processing_init(uint32_t now_ms);
void fft_processing_task(void *pvParameters);
void fft_processing_snapshot(detector_state_t *out_state);
bool fft_processing_get_test_mode(void);
void fft_processing_set_test_mode(bool activar, uint32_t now_ms);
void fft_processing_update_boot_button(uint32_t now_ms);
void fft_processing_update_demo(uint32_t now_ms);
void fft_processing_handle_alarms(bool alarma_ultra_activa);

#endif
