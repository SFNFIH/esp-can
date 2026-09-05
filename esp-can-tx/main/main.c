/*
 * esp-can-tx — vehicle CAN -> ESP-NOW + SoftAP web (ESP32-S3)
 *
 * FreeRTOS pipeline:
 *   TWAI ISR  --notify-->  can_rx_task  --queues-->  espnow_tx / web_feed
 *   ESP-NOW RX ISR -------> inject_queue --> can_inject_task
 *   Soft timer (1 Hz) ----> publish web stats
 *   Event group -----------> WIFI / ESP-NOW / TWAI ready bits
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

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
#include "freertos_app.h"
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
    uint32_t seq;
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
    int write_idx;
    int read_idx;
    int pool_depth;
    TaskHandle_t can_rx_task;
} twai_gateway_ctx_t;

static twai_gateway_ctx_t s_gw;
static EventGroupHandle_t s_app_events;
static QueueHandle_t s_espnow_queue;
static QueueHandle_t s_web_queue;
static QueueHandle_t s_inject_queue;
static SemaphoreHandle_t s_espnow_lock;
static TimerHandle_t s_stats_timer;

static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static uint32_t s_seq;
static volatile uint32_t s_rx_count;
static volatile uint32_t s_fwd_ok;
static volatile uint32_t s_fwd_fail;
static volatile uint32_t s_drop_count;
static volatile uint32_t s_inject_ok;
static volatile uint32_t s_inject_fail;
static volatile uint32_t s_inject_drop;

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

static void stats_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    publish_stats();
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

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if (!data || len != (int)sizeof(can_espnow_frame_t)) {
        return;
    }
    const can_espnow_frame_t *pkt = (const can_espnow_frame_t *)data;
    if (!can_espnow_frame_valid(pkt)) {
        return;
    }
    if (!s_inject_queue || xQueueSend(s_inject_queue, pkt, 0) != pdTRUE) {
        s_inject_drop++;
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
    xEventGroupSetBits(s_app_events, APP_EVT_WIFI_READY);

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
    xEventGroupSetBits(s_app_events, APP_EVT_PEER_CONFIGURED);

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "espnow");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(espnow_send_cb), TAG, "send cb");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(espnow_recv_cb), TAG, "recv cb");

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
    xEventGroupSetBits(s_app_events, APP_EVT_ESPNOW_READY);

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

/* ISR: fill pool slot, wake can_rx_task via Task Notification (no busy poll). */
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

    if (ctx->can_rx_task) {
        vTaskNotifyGiveFromISR(ctx->can_rx_task, &woken);
    }
    return woken == pdTRUE;
}

static esp_err_t twai_gateway_init(twai_gateway_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->pool_depth = CAN_QUEUE_LEN;
    ctx->free_pool_sem = xSemaphoreCreateCounting(ctx->pool_depth, ctx->pool_depth);
    ctx->rx_pool = calloc(ctx->pool_depth, sizeof(twai_pool_slot_t));
    if (!ctx->free_pool_sem || !ctx->rx_pool) {
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
    xEventGroupSetBits(s_app_events, APP_EVT_TWAI_READY);

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

static void queue_or_drop(QueueHandle_t q, const can_queued_frame_t *qf)
{
    if (xQueueSend(q, qf, 0) != pdTRUE) {
        s_drop_count++;
    }
}

/*
 * Highest-priority consumer: drain TWAI pool under TaskNotify, fan-out to
 * ESP-NOW and web queues so wireless / HTTP never block CAN ingest.
 */
static void can_rx_task(void *arg)
{
    twai_gateway_ctx_t *ctx = (twai_gateway_ctx_t *)arg;

    xEventGroupWaitBits(s_app_events, APP_EVT_ALL_READY, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "can_rx_task running (prio=%d)", APP_PRIO_CAN_RX);

    while (1) {
        /* Each ISR Give increments the notification count (= frames ready). */
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (n--) {
            twai_frame_t *frame = &ctx->rx_pool[ctx->read_idx].frame;
            can_queued_frame_t qf = {0};
            qf.id = frame->header.id;
            qf.seq = s_seq++;
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

            queue_or_drop(s_espnow_queue, &qf);
            queue_or_drop(s_web_queue, &qf);
        }
    }
}

static void espnow_tx_task(void *arg)
{
    (void)arg;
    can_queued_frame_t frame;

    xEventGroupWaitBits(s_app_events, APP_EVT_ESPNOW_READY | APP_EVT_PEER_CONFIGURED,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "espnow_tx_task running (prio=%d)", APP_PRIO_ESPNOW_TX);

    while (1) {
        if (xQueueReceive(s_espnow_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = espnow_forward_frame(&frame, frame.seq);
        uint32_t total = s_fwd_ok + s_fwd_fail;
        if ((LOG_EVERY_N == 0) || (total % LOG_EVERY_N == 0)) {
            ESP_LOGI(TAG,
                     "CAN id=0x%lX dlc=%u | rx=%lu ok=%lu fail=%lu drop=%lu (%s)",
                     (unsigned long)frame.id, frame.dlc,
                     (unsigned long)s_rx_count, (unsigned long)s_fwd_ok,
                     (unsigned long)s_fwd_fail, (unsigned long)s_drop_count,
                     esp_err_to_name(err));
        }
    }
}

static void web_feed_task(void *arg)
{
    (void)arg;
    can_queued_frame_t frame;

    ESP_LOGI(TAG, "web_feed_task running (prio=%d)", APP_PRIO_WEB_FEED);

    while (1) {
        if (xQueueReceive(s_web_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        can_monitor_frame_t mon = {
            .id = frame.id,
            .dlc = frame.dlc,
            .flags = frame.flags,
            .seq = frame.seq,
            .ts_us = esp_timer_get_time(),
        };
        memcpy(mon.data, frame.data, CAN_ESPNOW_MAX_DATA);
        web_monitor_publish_frame(&mon);
    }
}

static void can_inject_task(void *arg)
{
    (void)arg;
    can_espnow_frame_t pkt;

    xEventGroupWaitBits(s_app_events, APP_EVT_TWAI_READY, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "can_inject_task running (prio=%d)", APP_PRIO_CAN_INJECT);

    while (1) {
        if (xQueueReceive(s_inject_queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

#if CONFIG_EXAMPLE_TWAI_LISTEN_ONLY
        ESP_LOGW(TAG, "Ignore inject id=0x%lX (listen-only on; disable EXAMPLE_TWAI_LISTEN_ONLY to TX)",
                 (unsigned long)pkt.id);
        s_inject_fail++;
        continue;
#else
        uint8_t data[CAN_ESPNOW_MAX_DATA] = {0};
        if (pkt.dlc > CAN_ESPNOW_MAX_DATA) {
            pkt.dlc = CAN_ESPNOW_MAX_DATA;
        }
        memcpy(data, pkt.data, pkt.dlc);

        twai_frame_t frame = {
            .header = {
                .id = pkt.id,
                .dlc = pkt.dlc,
                .ide = (pkt.flags & CAN_ESPNOW_FLAG_EXT) ? 1 : 0,
                .rtr = (pkt.flags & CAN_ESPNOW_FLAG_RTR) ? 1 : 0,
            },
            .buffer = data,
            .buffer_len = pkt.dlc,
        };

        esp_err_t err = twai_node_transmit(s_gw.node_hdl, &frame, 100);
        if (err == ESP_OK) {
            s_inject_ok++;
            ESP_LOGI(TAG, "Injected to CAN id=0x%lX dlc=%u", (unsigned long)pkt.id, pkt.dlc);
        } else {
            s_inject_fail++;
            ESP_LOGW(TAG, "Inject failed id=0x%lX: %s", (unsigned long)pkt.id, esp_err_to_name(err));
        }
#endif
    }
}

void app_main(void)
{
    printf("=================== ESP-CAN-TX (FreeRTOS pipeline) ===================\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_app_events = xEventGroupCreate();
    s_espnow_queue = xQueueCreate(CAN_QUEUE_LEN, sizeof(can_queued_frame_t));
    s_web_queue = xQueueCreate(CAN_QUEUE_LEN, sizeof(can_queued_frame_t));
    s_inject_queue = xQueueCreate(16, sizeof(can_espnow_frame_t));
    assert(s_app_events && s_espnow_queue && s_web_queue && s_inject_queue);

    ESP_ERROR_CHECK(wifi_ap_espnow_init());
    ESP_ERROR_CHECK(web_monitor_start());
    ESP_ERROR_CHECK(twai_gateway_init(&s_gw));

    assert(xTaskCreate(can_rx_task, "can_rx", APP_STACK_CAN_RX, &s_gw,
                       APP_PRIO_CAN_RX, &s_gw.can_rx_task) == pdPASS);
    assert(xTaskCreate(espnow_tx_task, "espnow_tx", APP_STACK_ESPNOW_TX, NULL,
                       APP_PRIO_ESPNOW_TX, NULL) == pdPASS);
    assert(xTaskCreate(web_feed_task, "web_feed", APP_STACK_WEB_FEED, NULL,
                       APP_PRIO_WEB_FEED, NULL) == pdPASS);
    assert(xTaskCreate(can_inject_task, "can_inject", APP_STACK_CAN_INJECT, NULL,
                       APP_PRIO_CAN_INJECT, NULL) == pdPASS);

    s_stats_timer = xTimerCreate("stats", pdMS_TO_TICKS(APP_STATS_PERIOD_MS),
                                 pdTRUE, NULL, stats_timer_cb);
    assert(s_stats_timer);
    assert(xTimerStart(s_stats_timer, 0) == pdPASS);

    EventBits_t ready = xEventGroupWaitBits(s_app_events, APP_EVT_ALL_READY,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "App ready bits=0x%lx", (unsigned long)ready);
    ESP_LOGI(TAG, "Running. Join SoftAP and open http://192.168.4.1/");
#if CONFIG_EXAMPLE_TWAI_LISTEN_ONLY
    ESP_LOGW(TAG, "Listen-only ON: RX 'can send' frames will NOT be injected to vehicle CAN");
#endif
}
