#include "ble_comm.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);

#define TAG "BLE"
#define BLE_DEVICE_NAME "SOLEAC"
#define BLE_NOTIFY_MS 500

typedef struct {
    char data[192];
    uint16_t len;
} ble_telemetry_pkt_t;

static const ble_uuid128_t soleac_svc_uuid =
    BLE_UUID128_INIT(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                      0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t soleac_chr_telemetria_uuid =
    BLE_UUID128_INIT(0xf1, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                      0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t soleac_adv_uuids[] = {
    BLE_UUID128_INIT(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                     0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12),
};

static uint16_t ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t ble_telemetria_val_handle = 0;
static bool ble_telemetria_subscrita = false;
static uint8_t ble_own_addr_type = 0;
static uint32_t ultimo_notify_ble_ms = 0;
static SemaphoreHandle_t ble_pkt_mutex = NULL;
static ble_telemetry_pkt_t g_ble_telemetry_pkt = {0};
static portMUX_TYPE ble_command_mux = portMUX_INITIALIZER_UNLOCKED;
static bool ble_demo_command_pending = false;
static bool ble_demo_command_value = false;

static const ble_uuid128_t soleac_chr_comando_uuid =
    BLE_UUID128_INIT(0xf2, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                     0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static int ble_gatt_comando_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    uint8_t command[16] = {0};
    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0 || length >= sizeof(command) ||
        os_mbuf_copydata(ctxt->om, 0, length, command) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    bool value;
    if (length == 7 && memcmp(command, "demo_on", 7) == 0) {
        value = true;
    } else if (length == 8 && memcmp(command, "demo_off", 8) == 0) {
        value = false;
    } else {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    portENTER_CRITICAL(&ble_command_mux);
    ble_demo_command_value = value;
    ble_demo_command_pending = true;
    portEXIT_CRITICAL(&ble_command_mux);
    ESP_LOGI(TAG, "BLE: comando demo recibido: %s", value ? "ON" : "OFF");
    return 0;
}

static ble_telemetry_pkt_t ble_build_telemetry_pkt(const detector_state_t *state, bool alarma_activa)
{
    ble_telemetry_pkt_t pkt = {0};
    int written = snprintf(pkt.data, sizeof(pkt.data),
                            "{\"audio\":%.1f,\"hf\":%.1f,\"threshold\":%.1f,\"alarm\":%s,\"demo\":%s}",
                            state->nivel_audio, state->nivel_alta_frecuencia,
                            state->umbral_ultrasonido,
                            alarma_activa ? "true" : "false",
                            state->modo_prueba_activo ? "true" : "false");
    pkt.len = (written > 0 && (size_t)written < sizeof(pkt.data)) ? (uint16_t)written : 0;

    return pkt;
}

static void ble_store_latest_pkt(const ble_telemetry_pkt_t *pkt)
{
    xSemaphoreTake(ble_pkt_mutex, portMAX_DELAY);
    g_ble_telemetry_pkt = *pkt;
    xSemaphoreGive(ble_pkt_mutex);
}

static int ble_gatt_telemetria_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    ble_telemetry_pkt_t pkt;
    int rc;

    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    xSemaphoreTake(ble_pkt_mutex, portMAX_DELAY);
    pkt = g_ble_telemetry_pkt;
    xSemaphoreGive(ble_pkt_mutex);

    rc = os_mbuf_append(ctxt->om, pkt.data, pkt.len);
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def soleac_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &soleac_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &soleac_chr_telemetria_uuid.u,
                .access_cb = ble_gatt_telemetria_access_cb,
                .val_handle = &ble_telemetria_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &soleac_chr_comando_uuid.u,
                .access_cb = ble_gatt_comando_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                0,
            },
        },
    },
    {
        0,
    },
};

static void ble_iniciar_advertising(void);

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ble_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "BLE: cliente conectado");
            } else {
                ESP_LOGW(TAG, "BLE: fallo de conexion, reintentando advertising");
                ble_iniciar_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE: cliente desconectado (motivo %d)", event->disconnect.reason);
            ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_telemetria_subscrita = false;
            ble_iniciar_advertising();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ble_iniciar_advertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == ble_telemetria_val_handle) {
                ble_telemetria_subscrita = event->subscribe.cur_notify;
                ESP_LOGI(
                    TAG,
                    "BLE: notificaciones de telemetria %s",
                    ble_telemetria_subscrita ? "activadas" : "desactivadas");
            }
            return 0;

        default:
            return 0;
    }
}

static void ble_iniciar_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    struct ble_gap_adv_params adv_params = {0};
    int rc;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = soleac_adv_uuids;
    fields.num_uuids128 = sizeof(soleac_adv_uuids) / sizeof(soleac_adv_uuids[0]);
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE: fallo al configurar advertising (%d)", rc);
        return;
    }

    rsp_fields.name = (const uint8_t *)BLE_DEVICE_NAME;
    rsp_fields.name_len = strlen(BLE_DEVICE_NAME);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE: fallo al configurar scan response (%d)", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(
        ble_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE: fallo al iniciar advertising (%d)", rc);
    } else {
        ESP_LOGI(TAG, "BLE: advertising activo como '%s'", BLE_DEVICE_NAME);
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &ble_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE: fallo al inferir tipo de direccion (%d)", rc);
        return;
    }
    ble_iniciar_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE: host reseteado (motivo %d)", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE: tarea del host NimBLE iniciada");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_init_internal(void)
{
    esp_err_t ret;
    int rc;

    ble_pkt_mutex = xSemaphoreCreateMutex();
    if (ble_pkt_mutex == NULL) {
        ESP_LOGE(TAG, "BLE: no se pudo crear el mutex de telemetria");
        return ESP_ERR_NO_MEM;
    }

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE: fallo nimble_port_init (%s)", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();
    rc = ble_gatts_count_cfg(soleac_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE: fallo al contar configuracion GATT (%d)", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(soleac_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE: fallo al registrar servicios GATT (%d)", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE: fallo al fijar nombre del dispositivo (%d)", rc);
    }

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE listo. Anunciando como '%s'", BLE_DEVICE_NAME);
    return ESP_OK;
}

static void ble_notify_telemetria_internal(uint32_t now_ms, const detector_state_t *state, bool alarma_activa)
{
    ble_telemetry_pkt_t pkt = ble_build_telemetry_pkt(state, alarma_activa);
    struct os_mbuf *om;
    int rc;

    ble_store_latest_pkt(&pkt);

    if (ble_conn_handle == BLE_HS_CONN_HANDLE_NONE || !ble_telemetria_subscrita) {
        return;
    }
    if ((now_ms - ultimo_notify_ble_ms) < BLE_NOTIFY_MS) {
        return;
    }
    ultimo_notify_ble_ms = now_ms;

    om = ble_hs_mbuf_from_flat(pkt.data, pkt.len);
    if (om == NULL) {
        ESP_LOGW(TAG, "BLE: no se pudo asignar mbuf para notificar");
        return;
    }

    rc = ble_gattc_notify_custom(ble_conn_handle, ble_telemetria_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE: fallo al notificar telemetria (%d)", rc);
    }
}

esp_err_t ble_comm_init(void)
{
    return ble_init_internal();
}

void ble_comm_notify_telemetria(uint32_t now_ms, const detector_state_t *state, bool alarma_activa)
{
    ble_notify_telemetria_internal(now_ms, state, alarma_activa);
}

bool ble_comm_take_demo_command(bool *activar)
{
    bool pending;
    portENTER_CRITICAL(&ble_command_mux);
    pending = ble_demo_command_pending;
    if (pending) {
        *activar = ble_demo_command_value;
        ble_demo_command_pending = false;
    }
    portEXIT_CRITICAL(&ble_command_mux);
    return pending;
}
