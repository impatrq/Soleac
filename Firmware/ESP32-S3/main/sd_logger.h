#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "fft_processing.h"

esp_err_t sd_logger_init(void);
void sd_logger_enqueue(uint32_t uptime_ms, const detector_state_t *state, bool alarm_active);

#endif
