/**
 * @file bridge_protocol.h
 * @brief Shared HID packet protocol used over the SDIO bus between
 *        the ESP32-P4 (master) and ESP32-C6 (slave).
 *
 * Packet layout (fixed 10 bytes):
 *   [0]     magic   = 0xAB
 *   [1]     type    = BRIDGE_PKT_KEYBOARD | BRIDGE_PKT_MOUSE
 *   [2]     len     = number of valid bytes in data[]
 *   [3..9]  data    = raw HID boot-report bytes (up to 7 usable)
 */
#pragma once
#include <stdint.h>

#define BRIDGE_MAGIC          0xAB
#define BRIDGE_PKT_KEYBOARD   0x01
#define BRIDGE_PKT_MOUSE      0x02

/* Keyboard boot-protocol report: modifier(1) + reserved(1) + keycodes(6) = 8 bytes */
#define KBD_REPORT_LEN        8
/* Mouse boot-protocol report: buttons(1) + X(1) + Y(1) + wheel(1) = 4 bytes */
#define MOUSE_REPORT_LEN      4

#define BRIDGE_DATA_MAX       8   /* max payload bytes */
#define BRIDGE_PKT_SIZE       12  /* total wire size (align to 4 bytes) */

typedef struct __attribute__((packed)) {
    uint8_t magic;              /* 0xAB */
    uint8_t type;               /* BRIDGE_PKT_KEYBOARD / BRIDGE_PKT_MOUSE */
    uint8_t len;                /* valid bytes in data[] */
    uint8_t reserved;           /* padding */
    uint8_t data[BRIDGE_DATA_MAX]; /* HID report payload */
} bridge_pkt_t;

_Static_assert(sizeof(bridge_pkt_t) == BRIDGE_PKT_SIZE, "bridge_pkt_t size mismatch");
