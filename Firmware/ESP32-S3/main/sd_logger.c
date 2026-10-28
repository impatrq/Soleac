#include "sd_logger.h"

#include <stdio.h>
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define TAG "SD_LOGGER"
#define SD_MOUNT_POINT "/sdcard"
#define SD_LOG_FILE SD_MOUNT_POINT "/soleac.csv"
#define SD_RETRY_MS 5000U
#define SD_QUEUE_LENGTH 8U

typedef struct {
    uint32_t uptime_ms;
    uint32_t fft_sequence;
    float frequency_hz, hf_level, threshold, mq9_mv, mq9_pct, ma40_mv;
    uint8_t flags;
} sd_log_record_t;

static QueueHandle_t sd_log_queue;
static const char csv_header[] =
    "uptime_ms,mems_ok,calibrated,frequency_hz,hf_level,hf_threshold,fft_detect,mq9_connected,mq9_mv,mq9_pct,ma40_mv,ma40_detect,alarm,fft_sequence\n";

static esp_err_t sd_mount(sdmmc_card_t **card)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_11;
    slot.cmd = GPIO_NUM_10;
    slot.d0 = GPIO_NUM_9;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
    };
    return esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &config, card);
}

static bool sd_open_log(sdmmc_card_t **card, FILE **file)
{
    esp_err_t err = sd_mount(card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "microSD no disponible; reintento: %s", esp_err_to_name(err));
        return false;
    }
    *file = fopen(SD_LOG_FILE, "a+");
    if (*file == NULL) {
        ESP_LOGE(TAG, "No se pudo abrir %s", SD_LOG_FILE);
        (void)esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, *card);
        *card = NULL;
        return false;
    }
    if (fseek(*file, 0, SEEK_END) == 0 && ftell(*file) == 0 &&
        (fputs(csv_header, *file) < 0 || fflush(*file) != 0)) {
        ESP_LOGE(TAG, "No se pudo escribir el encabezado CSV");
        fclose(*file);
        *file = NULL;
        (void)esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, *card);
        *card = NULL;
        return false;
    }
    ESP_LOGI(TAG, "microSD montada; archivo: %s", SD_LOG_FILE);
    sdmmc_card_print_info(stdout, *card);
    return true;
}

static void sd_logger_task(void *arg)
{
    (void)arg;
    sdmmc_card_t *card = NULL;
    FILE *file = NULL;
    uint32_t last_retry_ms = 0;
    sd_log_record_t r;
    for (;;) {
        if (file == NULL) {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (last_retry_ms == 0 || now - last_retry_ms >= SD_RETRY_MS) {
                last_retry_ms = now;
                (void)sd_open_log(&card, &file);
            }
            if (file == NULL) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
        }
        if (xQueueReceive(sd_log_queue, &r, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        int n = fprintf(file, "%u,%u,%u,%.1f,%.3f,%.3f,%u,%u,%.1f,%.2f,%.1f,%u,%u,%u\n",
                        (unsigned)r.uptime_ms, (r.flags >> 0) & 1U, (r.flags >> 1) & 1U,
                        r.frequency_hz, r.hf_level, r.threshold, (r.flags >> 2) & 1U,
                        (r.flags >> 3) & 1U, r.mq9_mv, r.mq9_pct, r.ma40_mv,
                        (r.flags >> 4) & 1U, (r.flags >> 5) & 1U, (unsigned)r.fft_sequence);
        if (n < 0 || fflush(file) != 0) {
            ESP_LOGW(TAG, "Fallo de escritura; desmontando y esperando la tarjeta");
            fclose(file);
            file = NULL;
            (void)esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
            card = NULL;
            last_retry_ms = 0;
        }
    }
}

esp_err_t sd_logger_init(void)
{
    if (sd_log_queue != NULL) return ESP_OK;
    sd_log_queue = xQueueCreate(SD_QUEUE_LENGTH, sizeof(sd_log_record_t));
    if (sd_log_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(sd_logger_task, "sd_logger", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(sd_log_queue);
        sd_log_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sd_logger_enqueue(uint32_t uptime_ms, const detector_state_t *state, bool alarm_active)
{
    if (sd_log_queue == NULL || state == NULL) return;
    const sd_log_record_t r = {
        .uptime_ms = uptime_ms, .fft_sequence = state->fft_sequence,
        .frequency_hz = state->frecuencia_dominante_hz, .hf_level = state->nivel_alta_frecuencia,
        .threshold = state->umbral_ultrasonido, .mq9_mv = state->mq2_mv,
        .mq9_pct = state->mq2_pct, .ma40_mv = state->ma40_mv,
        .flags = (uint8_t)((state->mems_capture_status == MEMS_CAPTURE_OK ? 1U : 0U) |
                           (state->baseline_lista ? 2U : 0U) |
                           (state->fft_detecta_ultrasonido ? 4U : 0U) |
                           (state->mq2_conectado ? 8U : 0U) |
                           (state->ma40_detecta_ultrasonido ? 16U : 0U) |
                           (alarm_active ? 32U : 0U)),
    };
    (void)xQueueSend(sd_log_queue, &r, 0);
}
