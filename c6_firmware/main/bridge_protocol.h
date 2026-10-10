/**
 * @file bridge_protocol.h  (C6 copy)
 * @brief Shared HID packet protocol (identical to P4 version).
 *
 * Keep byte-for-byte identical with p4_firmware/main/bridge_protocol.h.
 */
#pragma once
#include <stdint.h>

#define BRIDGE_MAGIC          0xAB
#define BRIDGE_PKT_KEYBOARD   0x01
#define BRIDGE_PKT_MOUSE      0x02
#define BRIDGE_PKT_CONSUMER   0x03

#define KBD_REPORT_LEN        8
#define MOUSE_REPORT_LEN      4

#define BRIDGE_DATA_MAX       8
#define BRIDGE_PKT_SIZE       12

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t type;
    uint8_t len;
    uint8_t reserved;
    uint8_t data[BRIDGE_DATA_MAX];
} bridge_pkt_t;

_Static_assert(sizeof(bridge_pkt_t) == BRIDGE_PKT_SIZE, "bridge_pkt_t size mismatch");
