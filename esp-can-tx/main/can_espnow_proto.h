/*
 * Shared CAN-over-ESP-NOW frame format for esp-can-tx / esp-can-rx.
 * Keep both copies identical.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_ESPNOW_MAGIC        0xCAu
#define CAN_ESPNOW_VERSION      0x01
#define CAN_ESPNOW_MAX_DATA     8

#define CAN_ESPNOW_FLAG_EXT     (1u << 0)
#define CAN_ESPNOW_FLAG_RTR     (1u << 1)

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  flags;
    uint8_t  dlc;
    uint32_t id;
    uint32_t seq;
    uint8_t  data[CAN_ESPNOW_MAX_DATA];
} can_espnow_frame_t;

static inline bool can_espnow_frame_valid(const can_espnow_frame_t *f)
{
    return f && f->magic == CAN_ESPNOW_MAGIC && f->version == CAN_ESPNOW_VERSION && f->dlc <= CAN_ESPNOW_MAX_DATA;
}

#ifdef __cplusplus
}
#endif
