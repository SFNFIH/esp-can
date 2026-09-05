/*
 * esp-can-tx — vehicle CAN -> ESP-NOW + SoftAP web monitor (ESP32-S3)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_twai.h"
#include "esp_twai_onchip.h"

#include "can_espnow_proto.h"
#include "web_monitor.h"

#define TWAI_TX_GPIO    CONFIG_EXAMPLE_TWAI_TX_GPIO
#define TWAI_RX_GPIO    CONFIG_EXAMPLE_TWAI_RX_GPIO
#define TWAI_BITRATE    CONFIG_EXAMPLE_TWAI_BITRATE
#define CAN_QUEUE_LEN   CONFIG_EXAMPLE_CAN_QUEUE_LEN
#define WIFI_CHANNEL    CONFIG_EXAMPLE_ESPNOW_CHANNEL
#define LOG_EVERY_N     CONFIG_EXAMPLE_LOG_EVERY_N_FRAMES

static const char *TAG = "esp_can_tx";

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t flags;
    uint8_t data[CAN_ESPNOW_MAX_DATA];
} can_queued_frame_t;

typedef struct {
    twai_frame_t frame;
    uint8_t data[TWAI_FRAME_MAX_LEN];
} twai_pool_slot_t;

typedef struct {
    twai_node_handle_t node_hdl;
    twai_pool_slot_t *rx_pool;
    SemaphoreHandle_t free_pool_sem;
    SemaphoreHandle_t rx_ready_sem;
    QueueHandle_t fwd_queue;
    int write_idx;
    int read_idx;
    int pool_depth;
} twai_gateway_ctx_t;

static twai_gateway_ctx_t s_gw;
static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static SemaphoreHandle_t s_espnow_lock;
static uint32_t s_seq;
static uint32_t s_rx_count;
static uint32_t s_fwd_ok;
static uint32_t s_fwd_fail;
static uint32_t s_drop_count;

static int parse_mac_str(const char *str, uint8_t mac[ESP_NOW_ETH_ALEN])
{
    unsigned int b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
    return 0;
}

static void log_mac(const char *label, const uint8_t *mac)
{
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void publish_stats(void)
{
    can_monitor_stats_t st = {
        .rx_count = s_rx_count,
        .fwd_ok = s_fwd_ok,
        .fwd_fail = s_fwd_fail,
        .drop_count = s_drop_count,
    };
    web_monitor_set_stats(&st);
}

static void espnow_send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_fwd_ok++;
    } else {
        s_fwd_fail++;
    }
    if (s_espnow_lock) {
        xSemaphoreGive(s_espnow_lock);
    }
}

static esp_err_t wifi_ap_espnow_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event");

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, CONFIG_EXAMPLE_WIFI_AP_SSID, sizeof(ap.ap.ssid));
    strncpy((char *)ap.ap.password, CONFIG_EXAMPLE_WIFI_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(CONFIG_EXAMPLE_WIFI_AP_SSID);
    ap.ap.channel = WIFI_CHANNEL;
    ap.ap.max_connection = 4;
    ap.ap.authmode = (strlen(CONFIG_EXAMPLE_WIFI_AP_PASSWORD) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG, "channel");

    uint8_t ap_mac[6], sta_mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, ap_mac), TAG, "ap mac");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, sta_mac), TAG, "sta mac");
    log_mac("SoftAP MAC:", ap_mac);
    log_mac("STA MAC:", sta_mac);

    if (parse_mac_str(CONFIG_EXAMPLE_ESPNOW_PEER_MAC, s_peer_mac) != 0) {
        ESP_LOGE(TAG, "Invalid peer MAC: %s", CONFIG_EXAMPLE_ESPNOW_PEER_MAC);
        return ESP_ERR_INVALID_ARG;
    }
    log_mac("ESP-NOW peer:", s_peer_mac);

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "espnow");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(espnow_send_cb), TAG, "send cb");

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_peer_mac, ESP_NOW_ETH_ALEN);
    peer.channel = WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_peer_mac)) {
        ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add peer");
    }

    s_espnow_lock = xSemaphoreCreateBinary();
    if (!s_espnow_lock) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_espnow_lock);

    ESP_LOGI(TAG, "SoftAP SSID='%s' pass='%s' ch=%d",
             CONFIG_EXAMPLE_WIFI_AP_SSID, CONFIG_EXAMPLE_WIFI_AP_PASSWORD, WIFI_CHANNEL);
    ESP_LOGI(TAG, "Open http://192.168.4.1/ after joining hotspot");
    return ESP_OK;
}

static esp_err_t espnow_forward_frame(const can_queued_frame_t *qf, uint32_t seq)
{
    can_espnow_frame_t pkt = {
        .magic = CAN_ESPNOW_MAGIC,
        .version = CAN_ESPNOW_VERSION,
        .flags = qf->flags,
        .dlc = qf->dlc,
        .id = qf->id,
        .seq = seq,
    };
    memcpy(pkt.data, qf->data, CAN_ESPNOW_MAX_DATA);

    if (xSemaphoreTake(s_espnow_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        s_fwd_fail++;
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = esp_now_send(s_peer_mac, (const uint8_t *)&pkt, sizeof(pkt));
    if (err != ESP_OK) {
        s_fwd_fail++;
        xSemaphoreGive(s_espnow_lock);
    }
    return err;
}

static bool IRAM_ATTR twai_on_error_cb(twai_node_handle_t handle,
                                       const twai_error_event_data_t *edata,
                                       void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    ESP_EARLY_LOGW(TAG, "TWAI error: 0x%lx", (unsigned long)edata->err_flags.val);
    return false;
}

static bool IRAM_ATTR twai_on_state_change_cb(twai_node_handle_t handle,
                                              const twai_state_change_event_data_t *edata,
                                              void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
    ESP_EARLY_LOGI(TAG, "TWAI %s -> %s", names[edata->old_sta], names[edata->new_sta]);
    return false;
}

static bool IRAM_ATTR twai_on_rx_cb(twai_node_handle_t handle,
                                    const twai_rx_done_event_data_t *edata,
                                    void *user_ctx)
{
    (void)edata;
    twai_gateway_ctx_t *ctx = (twai_gateway_ctx_t *)user_ctx;
    BaseType_t woken = pdFALSE;

    if (xSemaphoreTakeFromISR(ctx->free_pool_sem, &woken) != pdTRUE) {
        s_drop_count++;
        return woken == pdTRUE;
    }

    twai_pool_slot_t *slot = &ctx->rx_pool[ctx->write_idx];
    if (twai_node_receive_from_isr(handle, &slot->frame) != ESP_OK) {
        xSemaphoreGiveFromISR(ctx->free_pool_sem, &woken);
        return woken == pdTRUE;
    }

    ctx->write_idx = (ctx->write_idx + 1) % ctx->pool_depth;
    s_rx_count++;
    xSemaphoreGiveFromISR(ctx->rx_ready_sem, &woken);
    return woken == pdTRUE;
}

static esp_err_t twai_gateway_init(twai_gateway_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->pool_depth = CAN_QUEUE_LEN;
    ctx->free_pool_sem = xSemaphoreCreateCounting(ctx->pool_depth, ctx->pool_depth);
    ctx->rx_ready_sem = xSemaphoreCreateCounting(ctx->pool_depth, 0);
    ctx->fwd_queue = xQueueCreate(ctx->pool_depth, sizeof(can_queued_frame_t));
    ctx->rx_pool = calloc(ctx->pool_depth, sizeof(twai_pool_slot_t));
    if (!ctx->free_pool_sem || !ctx->rx_ready_sem || !ctx->fwd_queue || !ctx->rx_pool) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < ctx->pool_depth; i++) {
        ctx->rx_pool[i].frame.buffer = ctx->rx_pool[i].data;
        ctx->rx_pool[i].frame.buffer_len = sizeof(ctx->rx_pool[i].data);
    }

    twai_onchip_node_config_t node_config = {
        .io_cfg = {
            .tx = TWAI_TX_GPIO,
            .rx = TWAI_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = TWAI_BITRATE,
        .timestamp_resolution_hz = 1000000,
#if CONFIG_EXAMPLE_TWAI_LISTEN_ONLY
        .flags.enable_listen_only = true,
#endif
    };

    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&node_config, &ctx->node_hdl), TAG, "twai new");

    twai_mask_filter_config_t open_filter = {
        .id = 0,
        .mask = 0,
        .is_ext = false,
    };
    ESP_RETURN_ON_ERROR(twai_node_config_mask_filter(ctx->node_hdl, 0, &open_filter), TAG, "filter");

    twai_event_callbacks_t cbs = {
        .on_rx_done = twai_on_rx_cb,
        .on_error = twai_on_error_cb,
        .on_state_change = twai_on_state_change_cb,
    };
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(ctx->node_hdl, &cbs, ctx), TAG, "cbs");
    ESP_RETURN_ON_ERROR(twai_node_enable(ctx->node_hdl), TAG, "enable");

    ESP_LOGI(TAG, "TWAI ready TX=%d RX=%d bitrate=%d listen_only=%d",
             TWAI_TX_GPIO, TWAI_RX_GPIO, TWAI_BITRATE,
#if CONFIG_EXAMPLE_TWAI_LISTEN_ONLY
             1
#else
             0
#endif
            );
    return ESP_OK;
}

static void can_consume_task(void *arg)
{
    twai_gateway_ctx_t *ctx = (twai_gateway_ctx_t *)arg;
    while (1) {
        if (xSemaphoreTake(ctx->rx_ready_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        twai_frame_t *frame = &ctx->rx_pool[ctx->read_idx].frame;
        can_queued_frame_t qf = {0};
        qf.id = frame->header.id;
        qf.dlc = (uint8_t)frame->header.dlc;
        if (qf.dlc > CAN_ESPNOW_MAX_DATA) {
            qf.dlc = CAN_ESPNOW_MAX_DATA;
        }
        if (frame->header.ide) {
            qf.flags |= CAN_ESPNOW_FLAG_EXT;
        }
        if (frame->header.rtr) {
            qf.flags |= CAN_ESPNOW_FLAG_RTR;
        }
        if (frame->buffer && qf.dlc) {
            memcpy(qf.data, frame->buffer, qf.dlc);
        }

        ctx->read_idx = (ctx->read_idx + 1) % ctx->pool_depth;
        xSemaphoreGive(ctx->free_pool_sem);

        if (xQueueSend(ctx->fwd_queue, &qf, 0) != pdTRUE) {
            s_drop_count++;
        }
    }
}

static void forward_task(void *arg)
{
    twai_gateway_ctx_t *ctx = (twai_gateway_ctx_t *)arg;
    can_queued_frame_t frame;

    while (1) {
        if (xQueueReceive(ctx->fwd_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t seq = s_seq++;
        can_monitor_frame_t mon = {
            .id = frame.id,
            .dlc = frame.dlc,
            .flags = frame.flags,
            .seq = seq,
            .ts_us = esp_timer_get_time(),
        };
        memcpy(mon.data, frame.data, CAN_ESPNOW_MAX_DATA);
        web_monitor_publish_frame(&mon);

        esp_err_t err = espnow_forward_frame(&frame, seq);
        uint32_t total = s_fwd_ok + s_fwd_fail;
        if ((LOG_EVERY_N == 0) || (total % LOG_EVERY_N == 0)) {
            ESP_LOGI(TAG,
                     "CAN id=0x%lX dlc=%u data=%02X %02X %02X %02X %02X %02X %02X %02X | "
                     "rx=%lu ok=%lu fail=%lu drop=%lu (%s)",
                     (unsigned long)frame.id, frame.dlc,
                     frame.data[0], frame.data[1], frame.data[2], frame.data[3],
                     frame.data[4], frame.data[5], frame.data[6], frame.data[7],
                     (unsigned long)s_rx_count, (unsigned long)s_fwd_ok,
                     (unsigned long)s_fwd_fail, (unsigned long)s_drop_count,
                     esp_err_to_name(err));
        }
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        publish_stats();
    }
}

void app_main(void)
{
    printf("=================== ESP-CAN-TX (CAN + ESP-NOW + Web) ===================\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(wifi_ap_espnow_init());
    ESP_ERROR_CHECK(web_monitor_start());
    ESP_ERROR_CHECK(twai_gateway_init(&s_gw));

    assert(xTaskCreate(can_consume_task, "can_consume", 4096, &s_gw, 12, NULL) == pdPASS);
    assert(xTaskCreate(forward_task, "forward", 4096, &s_gw, 10, NULL) == pdPASS);
    assert(xTaskCreate(stats_task, "stats", 3072, NULL, 5, NULL) == pdPASS);

    ESP_LOGI(TAG, "Running. Join SoftAP and open http://192.168.4.1/");
}
