#pragma once

/**
 * FreeRTOS layout for esp-can-rx (garage / PC-side).
 *
 * Priority (high → low):
 *   CAN_RX   — drain ESP-NOW RX queue, update ring, wake CLI watchers
 *   CLI      — UART console (blocks on line input)
 *   CAN_TX   — outbound ESP-NOW inject toward car gateway
 *   STATS    — soft-timer driven counters
 *
 * Sync primitives:
 *   EventGroup  — peer learned / Wi-Fi ready
 *   Queue       — ESP-NOW ISR→task, CLI→TX task
 *   TaskNotify  — wake can_rx_task; wake CLI on new frame (watch)
 *   Mutex       — frame ring + peer MAC
 *   Soft Timer  — 1 Hz statistics tick
 */

#include "freertos/FreeRTOS.h"

enum {
    APP_PRIO_CAN_RX = 6,
    APP_PRIO_CLI    = 5,
    APP_PRIO_CAN_TX = 4,
    APP_PRIO_STATS  = 2,
};

enum {
    APP_STACK_CAN_RX = 4096,
    APP_STACK_CLI    = 4096,
    APP_STACK_CAN_TX = 4096,
    APP_STACK_STATS  = 3072,
};

#define APP_EVT_WIFI_READY    (1U << 0)
#define APP_EVT_ESPNOW_READY  (1U << 1)
#define APP_EVT_PEER_LEARNED  (1U << 2)
#define APP_EVT_ALL_READY     (APP_EVT_WIFI_READY | APP_EVT_ESPNOW_READY)

#define APP_STATS_PERIOD_MS   5000

/* Task notification index 0: new CAN frame available for CLI watch */
#define APP_NOTIFY_FRAME      0
