/*
 * esp-can-rx — ESP-NOW receiver for vehicle CAN frames (ESP32-S3)
 *
 * Receives can_espnow_frame_t packets forwarded by esp-can-tx and logs them.
 * Extend this file later to drive actuators / local CAN / UART as needed.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"

#include "can_espnow_proto.h"

#define ESPNOW_CHANNEL      CONFIG_EXAMPLE_ESPNOW_CHANNEL
#define LOG_EVERY_N         CONFIG_EXAMPLE_LOG_EVERY_N_FRAMES
#define RX_QUEUE_LEN        64

static const char *TAG = "esp_can_rx";

static QueueHandle_t s_rx_queue;
static uint32_t s_rx_count;
static uint32_t s_rx_invalid;
static uint32_t s_rx_drop;

static void log_mac(const char *label, const uint8_t *mac)
{
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

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

    if (xQueueSend(s_rx_queue, pkt, 0) != pdTRUE) {
        s_rx_drop++;
        return;
    }
    s_rx_count++;
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

    uint8_t local_mac[ESP_NOW_ETH_ALEN];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, local_mac), TAG, "get mac");
    log_mac("Local STA MAC (set this on esp-can-tx peer):", local_mac);

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "espnow init");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(espnow_recv_cb), TAG, "recv cb");

    /* Allow broadcast / any peer; TX can send broadcast during bring-up. */
    esp_now_peer_info_t peer = {0};
    memset(peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(peer.peer_addr)) {
        ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add broadcast peer");
    }

    ESP_LOGI(TAG, "ESP-NOW ready, channel=%d", ESPNOW_CHANNEL);
    return ESP_OK;
}

static void rx_task(void *arg)
{
    (void)arg;
    can_espnow_frame_t frame;

    while (1) {
        if (xQueueReceive(s_rx_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bool log_it = (LOG_EVERY_N == 0) || (s_rx_count % (LOG_EVERY_N ? LOG_EVERY_N : 1) == 0);
        if (log_it) {
            ESP_LOGI(TAG,
                     "ESPNOW RX seq=%lu id=0x%lX dlc=%u flags=0x%02x "
                     "data=%02X %02X %02X %02X %02X %02X %02X %02X | "
                     "ok=%lu invalid=%lu drop=%lu",
                     (unsigned long)frame.seq,
                     (unsigned long)frame.id,
                     frame.dlc,
                     frame.flags,
                     frame.data[0], frame.data[1], frame.data[2], frame.data[3],
                     frame.data[4], frame.data[5], frame.data[6], frame.data[7],
                     (unsigned long)s_rx_count,
                     (unsigned long)s_rx_invalid,
                     (unsigned long)s_rx_drop);
        }
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "stats ok=%lu invalid=%lu drop=%lu",
                 (unsigned long)s_rx_count,
                 (unsigned long)s_rx_invalid,
                 (unsigned long)s_rx_drop);
    }
}

void app_main(void)
{
    printf("=================== ESP-CAN-RX (ESP-NOW CAN sink) ===================\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(can_espnow_frame_t));
    assert(s_rx_queue);

    ESP_ERROR_CHECK(wifi_espnow_init());

    BaseType_t ok;
    ok = xTaskCreate(rx_task, "espnow_rx", 4096, NULL, 10, NULL);
    assert(ok == pdPASS);
    ok = xTaskCreate(stats_task, "stats", 3072, NULL, 5, NULL);
    assert(ok == pdPASS);

    ESP_LOGI(TAG, "Waiting for CAN frames from esp-can-tx ...");
}
