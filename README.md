# USB-to-BLE Bridge — Waveshare ESP32-P4 + ESP32-C6

Stream USB keyboard/mouse inputs wirelessly over Bluetooth LE to a PC, with near-zero latency.

---

## Architecture

```
USB Hub / Keyboard / Mouse
          │ USB 2.0 HS (Host)
          ▼
   ┌─────────────────────────────────────────┐
   │          ESP32-P4 (p4_firmware)          │
   │                                          │
   │  USB Host HID task (Core 0)              │
   │    • enumerate kbd + mouse               │
   │    • parse boot-protocol reports         │
   │         │                                │
   │         ▼  bridge_pkt_t (queue depth=32) │
   │  SDIO Master task (Core 1)               │
   │    • SDMMC 4-bit @ 20 MHz               │
   │    • ESSL send_packet()                  │
   └──────────────┬──────────────────────────┘
                  │ Internal SDIO bus (4-bit)
                  ▼
   ┌─────────────────────────────────────────┐
   │          ESP32-C6 (c6_firmware)          │
   │                                          │
   │  SDIO Slave task (pri=7)                 │
   │    • receive bridge_pkt_t                │
   │         │                                │
   │         ▼  bridge_pkt_t (queue depth=32) │
   │  BLE Forward task (pri=5)                │
   │    • NimBLE HOGP notify                  │
   │    • Keyboard Report ID 1                │
   │    • Mouse    Report ID 2                │
   └──────────────┬──────────────────────────┘
                  │ BLE 5.3 (7.5 ms interval)
                  ▼
        PC / Laptop (BLE HID host)
```

---

## SDIO Pin Mapping (P4 → C6)

| Signal | P4 GPIO | C6 GPIO (fixed) |
|--------|---------|-----------------|
| CLK    | 18      | 19              |
| CMD    | 19      | 18              |
| D0     | 14      | 20              |
| D1     | 15      | 21              |
| D2     | 16      | 22              |
| D3     | 17      | 23              |

> Adjust `SDIO_*_GPIO` defines in `p4_firmware/main/sdio_master.c` if your Waveshare variant routes differently.

---

## Build & Flash

### Prerequisites

```bash
source /home/shajidur-rahman/esp/esp-idf/export.sh
```

### ESP32-P4 Firmware

```bash
cd p4_firmware
idf.py build
# Flash using esptool (early silicon workaround):
python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset \
  write_flash \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/usb_bt_bridge_p4.bin
```

### ESP32-C6 Firmware

```bash
cd c6_firmware
idf.py -p /dev/ttyACM0 build flash monitor
```

---

## Packet Protocol

`bridge_protocol.h` defines a 12-byte fixed-size packet:

```
Offset  Field      Value
──────────────────────────────────────────
  0     magic      0xAB
  1     type       0x01=Keyboard  0x02=Mouse
  2     len        payload length (8 or 4)
  3     reserved   0x00
 4–11   data       raw HID boot-protocol report
```

---

## BLE HID Reports

| Report | ID | Format                          |
|--------|----|---------------------------------|
| Keyboard | 1 | modifier(1) + rsvd(1) + keys(6) |
| Mouse    | 2 | buttons(1) + X(1) + Y(1) + wheel(1) |

Connection parameters: `itvl_min=6` (7.5 ms), `itvl_max=12` (15 ms), latency=0.

Pairing: Just Works (no passkey). Bonding is enabled so the host reconnects automatically.

---

## Project Structure

```
usb_bt_bridge/
├── p4_firmware/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   ├── components/
│   │   └── esp_serial_slave_link/   ← ESSL (copied from IDF test_apps)
│   └── main/
│       ├── main.c
│       ├── bridge_protocol.h
│       ├── usb_hid_host.[ch]
│       └── sdio_master.[ch]
└── c6_firmware/
    ├── CMakeLists.txt
    ├── sdkconfig.defaults
    ├── partitions.csv
    └── main/
        ├── main.c
        ├── bridge_protocol.h
        ├── sdio_slave_task.[ch]
        └── ble_hid.[ch]
```
