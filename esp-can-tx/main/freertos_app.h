#pragma once

/**
 * FreeRTOS layout for esp-can-tx (car-side gateway).
 *
 * Priority (high → low):
 *   CAN_RX   — drain TWAI ASAP, fan-out to ESP-NOW / web / inject path
 *   ESPNOW_TX — wireless send (may block briefly on Wi-Fi)
 *   WEB_FEED  — SoftAP HTTP buffer ingest
 *   CAN_INJECT — optional RX→vehicle CAN writes
 *   STATS     — soft-timer driven counters (idle-friendly)
 *
 * Sync primitives:
 *   EventGroup  — lifecycle / link readiness bits
 *   Queue       — ISR→task and task→task pipelines
 *   TaskNotify  — wake consumer without polling
 *   Mutex       — web ring / peer MAC shared state
 *   Soft Timer  — 1 Hz statistics tick
 */

#include "freertos/FreeRTOS.h"

enum {
    APP_PRIO_CAN_RX     = 6,
    APP_PRIO_ESPNOW_TX  = 5,
    APP_PRIO_WEB_FEED   = 4,
    APP_PRIO_CAN_INJECT = 4,
    APP_PRIO_STATS      = 2,
};

enum {
    APP_STACK_CAN_RX     = 4096,
    APP_STACK_ESPNOW_TX  = 4096,
    APP_STACK_WEB_FEED   = 3072,
    APP_STACK_CAN_INJECT = 4096,
    APP_STACK_STATS      = 3072,
};

/* Event group bits (s_app_events) */
#define APP_EVT_WIFI_READY       (1U << 0)
#define APP_EVT_ESPNOW_READY     (1U << 1)
#define APP_EVT_TWAI_READY       (1U << 2)
#define APP_EVT_PEER_CONFIGURED  (1U << 3)
#define APP_EVT_ALL_READY        (APP_EVT_WIFI_READY | APP_EVT_ESPNOW_READY | APP_EVT_TWAI_READY)

#define APP_STATS_PERIOD_MS      1000
