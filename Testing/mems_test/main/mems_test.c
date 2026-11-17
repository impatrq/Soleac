#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "MEMS_TEST"

// SOLEAC ESP32-S3 header: GPIO42 -> MEMS CLK, GPIO41 <- MEMS DATA.
#define MEMS_CLK_GPIO GPIO_NUM_42
#define MEMS_DATA_GPIO GPIO_NUM_41
#define MEMS_SELECT_SLOT I2S_PDM_SLOT_LEFT // SELECT conectado a GND

#define PDM_CLOCK_HZ 3072000U
#define PDM_DRIVER_RATE_HZ (PDM_CLOCK_HZ / 2U)
#define PCM_RATE_HZ 192000U
#define PDM_WORDS_PER_FRAME 1920U // 10 ms, exactamente 10 ciclos de 1 kHz
#define PDM_DECIMATION 16U
#define READ_TIMEOUT_MS 200U
#define REPORT_FRAMES 50U // 0.5 s entre informes

static i2s_chan_handle_t rx_channel;
static uint16_t pdm_words[PDM_WORDS_PER_FRAME];
static float pcm_samples[PDM_WORDS_PER_FRAME];

static esp_err_t read_full_frame(void)
{
    uint8_t *dst = (uint8_t *)pdm_words;
    size_t received = 0;
    const size_t target = sizeof(pdm_words);

    while (received < target) {
        size_t chunk = 0;
        esp_err_t err = i2s_channel_read(
            rx_channel, dst + received, target - received,
            &chunk, pdMS_TO_TICKS(READ_TIMEOUT_MS));
        if (err != ESP_OK) {
            return err;
        }
        if (chunk == 0) {
            return ESP_ERR_TIMEOUT;
        }
        received += chunk;
    }
    return ESP_OK;
}

static float goertzel_rms(const float *samples, size_t count, float frequency_hz)
{
    const float omega = 2.0f * (float)M_PI * frequency_hz / (float)PCM_RATE_HZ;
    const float coeff = 2.0f * cosf(omega);
    float q1 = 0.0f;
    float q2 = 0.0f;
    float mean = 0.0f;

    for (size_t i = 0; i < count; ++i) {
        mean += samples[i];
    }
    mean /= (float)count;

    for (size_t i = 0; i < count; ++i) {
        float q0 = (samples[i] - mean) + coeff * q1 - q2;
        q2 = q1;
        q1 = q0;
    }

    float power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
    if (power < 0.0f) {
        power = 0.0f;
    }
    // Goertzel sinusoid RMS, expressed in the PCM units used below.
    return sqrtf(2.0f * power) / (float)count;
}

static void init_mems_pdm(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_frame_num = PDM_WORDS_PER_FRAME;
    channel_config.dma_desc_num = 8;
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &rx_channel));

    i2s_pdm_rx_config_t pdm_config = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_DRIVER_RATE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MEMS_CLK_GPIO,
            .din = MEMS_DATA_GPIO,
            .invert_flags = {.clk_inv = false},
        },
    };
    pdm_config.slot_cfg.slot_mask = MEMS_SELECT_SLOT;

    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_channel, &pdm_config));
    ESP_ERROR_CHECK(gpio_set_pull_mode(MEMS_DATA_GPIO, GPIO_FLOATING));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_channel));
    vTaskDelay(pdMS_TO_TICKS(30)); // despertar del SPH0641
}

void app_main(void)
{
    ESP_LOGI(TAG, "SPH0641 PDM: CLK=GPIO%d DATA=GPIO%d SELECT=GND/LEFT",
             MEMS_CLK_GPIO, MEMS_DATA_GPIO);
    ESP_LOGI(TAG, "Reloj PDM=%u Hz; PCM=%u Hz; tono de prueba Goertzel=1 kHz",
             PDM_CLOCK_HZ, PCM_RATE_HZ);
    ESP_LOGI(TAG, "Deja el MEMS en silencio 3 s; luego acerca una fuente de tono de 1 kHz.");

    init_mems_pdm();
    uint64_t ones_acc = 0;
    double rms_acc = 0.0;
    double tone_1k_acc = 0.0;
    double tone_12k_acc = 0.0;
    double tone_40k_acc = 0.0;
    float tone_40k_peak = 0.0f;
    uint32_t good_frames = 0;
    uint32_t errors = 0;

    while (true) {
        esp_err_t err = read_full_frame();
        if (err != ESP_OK) {
            errors++;
            ESP_LOGW(TAG, "Lectura PDM fallida: %s (errores=%u)",
                     esp_err_to_name(err), errors);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        float mean = 0.0f;
        float square_sum = 0.0f;
        for (size_t i = 0; i < PDM_WORDS_PER_FRAME; ++i) {
            unsigned ones = (unsigned)__builtin_popcount((unsigned)pdm_words[i]);
            ones_acc += ones;
            int centered = (2 * (int)ones) - (int)PDM_DECIMATION;
            // Escala igual a la conversión usada por el detector principal.
            pcm_samples[i] = (float)(centered * 128);
            mean += pcm_samples[i];
        }
        mean /= (float)PDM_WORDS_PER_FRAME;
        for (size_t i = 0; i < PDM_WORDS_PER_FRAME; ++i) {
            float centered = pcm_samples[i] - mean;
            square_sum += centered * centered;
        }

        float rms = sqrtf(square_sum / (float)PDM_WORDS_PER_FRAME);
        float tone_1k = goertzel_rms(pcm_samples, PDM_WORDS_PER_FRAME, 1000.0f);
        float tone_12k = goertzel_rms(pcm_samples, PDM_WORDS_PER_FRAME, 12000.0f);
        float tone_40k = goertzel_rms(pcm_samples, PDM_WORDS_PER_FRAME, 40000.0f);
        // HC-SR04 emits a short 8-cycle burst (~200 us), so inspect 1 ms
        // windows and keep the largest 40 kHz response over each report period.
        for (size_t offset = 0; offset < PDM_WORDS_PER_FRAME; offset += 192U) {
            float burst_window = goertzel_rms(&pcm_samples[offset], 192U, 40000.0f);
            if (burst_window > tone_40k_peak) {
                tone_40k_peak = burst_window;
            }
        }
        rms_acc += rms;
        tone_1k_acc += tone_1k;
        tone_12k_acc += tone_12k;
        tone_40k_acc += tone_40k;
        good_frames++;

        if (good_frames >= REPORT_FRAMES) {
            double total_bits = (double)good_frames * PDM_WORDS_PER_FRAME * PDM_DECIMATION;
            float ones_pct = 100.0f * (float)((double)ones_acc / total_bits);
            ESP_LOGI(TAG,
                     "captura=OK unos=%.1f%% RMS=%.1f tono_1k=%.1f tono_12k=%.1f tono_40k=%.1f pico_40k=%.1f errores=%u",
                     ones_pct,
                     (float)(rms_acc / good_frames),
                     (float)(tone_1k_acc / good_frames),
                     (float)(tone_12k_acc / good_frames),
                     (float)(tone_40k_acc / good_frames), tone_40k_peak, errors);
            ones_acc = 0;
            rms_acc = 0.0;
            tone_1k_acc = 0.0;
            tone_12k_acc = 0.0;
            tone_40k_acc = 0.0;
            tone_40k_peak = 0.0f;
            good_frames = 0;
        }
    }
}
