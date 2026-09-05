/*
 * esp-can-rx — ESP-NOW CAN receive + UART CLI (ESP32-S3)
 *
 * FreeRTOS pipeline:
 *   ESP-NOW RX cb --queue+notify--> can_rx_task --> CLI ring / peer learn
 *   CLI "can send" --queue-------> can_tx_task --> ESP-NOW
 *   Soft timer (1 Hz) -----------> optional stats heartbeat log
 *   Event group ------------------> WIFI / ESP-NOW / peer-learned bits
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"

#include "can_espnow_proto.h"
#include "can_cli.h"
#include "freertos_app.h"

#define ESPNOW_CHANNEL      CONFIG_EXAMPLE_ESPNOW_CHANNEL
#define RX_QUEUE_LEN        64
#define TX_QUEUE_LEN        16

static const char *TAG = "esp_can_rx";

typedef struct {
    can_espnow_frame_t frame;
    uint8_t src_mac[ESP_NOW_ETH_ALEN];
} rx_item_t;

static EventGroupHandle_t s_app_events;
static QueueHandle_t s_rx_queue;
static QueueHandle_t s_tx_queue;
static TaskHandle_t s_can_rx_task;
static TimerHandle_t s_stats_timer;
static volatile uint32_t s_rx_queue_drop;
static volatile uint32_t s_rx_invalid;
static volatile uint32_t s_rx_ok;

static void log_mac(const char *label, const uint8_t *mac)
{
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void stats_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    /* Lightweight heartbeat — avoids a dedicated low-prio stats task. */
    if (s_rx_ok == 0 && s_rx_queue_drop == 0) {
        return;
    }
    EventBits_t bits = xEventGroupGetBits(s_app_events);
    ESP_LOGI(TAG, "stats rx_ok=%lu q_drop=%lu invalid=%lu peer=%s",
             (unsigned long)s_rx_ok,
             (unsigned long)s_rx_queue_drop,
             (unsigned long)s_rx_invalid,
             (bits & APP_EVT_PEER_LEARNED) ? "yes" : "no");
}

/* Called from Wi-Fi task context (not ISR) — use task-level FreeRTOS APIs. */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !data || len != (int)sizeof(can_espnow_frame_t)) {
        s_rx_invalid++;
        return;
    }

    const can_espnow_frame_t *pkt = (const can_espnow_frame_t *)data;
    if (!can_espnow_frame_valid(pkt)) {
        s_rx_invalid++;
        return;
    }

    rx_item_t item = {0};
    item.frame = *pkt;
    memcpy(item.src_mac, info->src_addr, ESP_NOW_ETH_ALEN);

    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        s_rx_queue_drop++;
        can_cli_note_drop();
        return;
    }
    if (s_can_rx_task) {
        xTaskNotifyGive(s_can_rx_task);
    }
}

static esp_err_t wifi_espnow_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG, "channel");
    xEventGroupSetBits(s_app_events, APP_EVT_WIFI_READY);

    uint8_t local_mac[ESP_NOW_ETH_ALEN];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, local_mac), TAG, "get mac");
    log_mac("本机 STA MAC（填到 esp-can-tx 对端）:", local_mac);

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "espnow init");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(espnow_recv_cb), TAG, "recv cb");

    esp_now_peer_info_t peer = {0};
    memset(peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(peer.peer_addr)) {
        ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add broadcast peer");
    }

    xEventGroupSetBits(s_app_events, APP_EVT_ESPNOW_READY);
    ESP_LOGI(TAG, "ESP-NOW ready, channel=%d", ESPNOW_CHANNEL);
    return ESP_OK;
}

static void can_rx_task(void *arg)
{
    (void)arg;
    rx_item_t item;

    xEventGroupWaitBits(s_app_events, APP_EVT_ALL_READY, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "can_rx_task running (prio=%d)", APP_PRIO_CAN_RX);

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (xQueueReceive(s_rx_queue, &item, 0) == pdTRUE) {
            s_rx_ok++;
            can_cli_on_frame(&item.frame, item.src_mac);
        }
    }
}

static void can_tx_task(void *arg)
{
    (void)arg;
    can_espnow_frame_t pkt;
    uint8_t peer[ESP_NOW_ETH_ALEN];

    xEventGroupWaitBits(s_app_events, APP_EVT_ESPNOW_READY, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "can_tx_task running (prio=%d)", APP_PRIO_CAN_TX);

    while (1) {
        if (xQueueReceive(s_tx_queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!can_cli_get_peer(peer)) {
            can_cli_note_tx_result(false);
            ESP_LOGW(TAG, "can_tx: no peer MAC yet");
            continue;
        }

        if (!esp_now_is_peer_exist(peer)) {
            esp_now_peer_info_t info = {0};
            memcpy(info.peer_addr, peer, ESP_NOW_ETH_ALEN);
            info.channel = ESPNOW_CHANNEL;
            info.ifidx = WIFI_IF_STA;
            info.encrypt = false;
            if (esp_now_add_peer(&info) != ESP_OK) {
                can_cli_note_tx_result(false);
                continue;
            }
        }

        esp_err_t err = esp_now_send(peer, (const uint8_t *)&pkt, sizeof(pkt));
        can_cli_note_tx_result(err == ESP_OK);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "TX inject id=0x%lX dlc=%u", (unsigned long)pkt.id, pkt.dlc);
        }
    }
}

void app_main(void)
{
    printf("=================== ESP-CAN-RX (FreeRTOS pipeline) ===================\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_app_events = xEventGroupCreate();
    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_item_t));
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(can_espnow_frame_t));
    assert(s_app_events && s_rx_queue && s_tx_queue);

    ESP_ERROR_CHECK(wifi_espnow_init());

    assert(xTaskCreate(can_rx_task, "can_rx", APP_STACK_CAN_RX, NULL,
                       APP_PRIO_CAN_RX, &s_can_rx_task) == pdPASS);
    assert(xTaskCreate(can_tx_task, "can_tx", APP_STACK_CAN_TX, NULL,
                       APP_PRIO_CAN_TX, NULL) == pdPASS);

    ESP_ERROR_CHECK(can_cli_start(s_app_events, s_tx_queue));

    s_stats_timer = xTimerCreate("stats", pdMS_TO_TICKS(APP_STATS_PERIOD_MS),
                                 pdTRUE, NULL, stats_timer_cb);
    assert(s_stats_timer);
    assert(xTimerStart(s_stats_timer, 0) == pdPASS);

    ESP_LOGI(TAG, "Ready. Use UART console commands (type help).");
}
