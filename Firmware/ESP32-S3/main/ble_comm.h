#ifndef BLE_COMM_H
#define BLE_COMM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "fft_processing.h"

esp_err_t ble_comm_init(void);
bool ble_comm_take_demo_command(bool *activar);
void ble_comm_notify_telemetria(uint32_t now_ms, const detector_state_t *state, bool alarma_activa);

#endif
