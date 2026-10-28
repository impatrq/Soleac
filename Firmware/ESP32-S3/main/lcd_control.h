#ifndef LCD_CONTROL_H
#define LCD_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "fft_processing.h"

esp_err_t lcd_control_init(void);
bool lcd_control_is_available(void);
esp_err_t lcd_control_run_diagnostics(void);
void lcd_control_render(uint32_t now_ms, const detector_state_t *state, bool alarma_ultra_activa);
bool lcd_control_touch_demo_requested(uint32_t now_ms);

#endif