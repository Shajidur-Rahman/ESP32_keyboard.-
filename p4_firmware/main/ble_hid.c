/**
 * @file ble_hid.c
 * @brief BLE HID Device (HOGP profile) for ESP32-C6 using Apache NimBLE.
 *
 * Implements a composite Keyboard + Mouse BLE HID peripheral.
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │  GAP:  Device name "ESP-HID-Bridge", connectable undirected adv  │
 * │  GATT services:                                                   │
 * │    • Generic Access       (0x1800)                                │
 * │    • Generic Attribute    (0x1801)                                │
 * │    • Battery              (0x180F) – fixed 100%                  │
 * │    • Device Information   (0x180A)                                │
 * │    • HID                  (0x1812)                                │
 * │        – HID Information  (0x2A4A)                               │
 * │        – Report Map       (0x2A4B)  composite kbd+mouse desc      │
 * │        – HID Control Pt   (0x2A4C)                               │
 * │        – Protocol Mode    (0x2A4E) → boot (0x00)                 │
 * │        – Keyboard Report  (0x2A4D) notify, Report Ref ID=1       │
 * │        – Mouse Report     (0x2A4D) notify, Report Ref ID=2       │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * Connection parameters negotiated for minimum latency:
 *   min/max interval = 6/12 (7.5 ms – 15 ms)
 *   slave latency    = 0
 *   supervision TO   = 200 (2 s)
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "nvs_flash.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_hid.h"

static const char *TAG = "ble_hid";

/* ─── Connection state ───────────────────────────────────────────── */
static volatile uint16_t g_conn_handle        = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t g_kbd_report_handle  = 0;
static volatile uint16_t g_mouse_report_handle = 0;
static volatile bool     g_kbd_notify_en      = false;
static volatile bool     g_mouse_notify_en    = false;

/* ─── HID Report Descriptor ─────────────────────────────────────────
 * Composite: Keyboard (Report ID 1) + Mouse (Report ID 2)            */
static const uint8_t hid_report_map[] = {
    /* ── Keyboard (Report ID 1) ── */
    0x05, 0x01,  /* Usage Page (Generic Desktop) */
    0x09, 0x06,  /* Usage (Keyboard) */
    0xA1, 0x01,  /* Collection (Application) */
    0x85, 0x01,  /*   Report ID (1) */
    /* Modifier byte */
    0x05, 0x07,  /*   Usage Page (Key Codes) */
    0x19, 0xE0,  /*   Usage Minimum (224) */
    0x29, 0xE7,  /*   Usage Maximum (231) */
    0x15, 0x00,  /*   Logical Minimum (0) */
    0x25, 0x01,  /*   Logical Maximum (1) */
    0x75, 0x01,  /*   Report Size (1) */
    0x95, 0x08,  /*   Report Count (8) */
    0x81, 0x02,  /*   Input (Data, Variable, Absolute) */
    /* Reserved byte */
    0x95, 0x01,  /*   Report Count (1) */
    0x75, 0x08,  /*   Report Size (8) */
    0x81, 0x01,  /*   Input (Constant) */
    /* Keycodes (6 bytes) */
    0x95, 0x06,  /*   Report Count (6) */
    0x75, 0x08,  /*   Report Size (8) */
    0x15, 0x00,  /*   Logical Minimum (0) */
    0x25, 0x65,  /*   Logical Maximum (101) */
    0x05, 0x07,  /*   Usage Page (Key Codes) */
    0x19, 0x00,  /*   Usage Minimum (0) */
    0x29, 0x65,  /*   Usage Maximum (101) */
    0x81, 0x00,  /*   Input (Data, Array, Absolute) */
    0xC0,        /* End Collection */

    /* ── Mouse (Report ID 2) ── */
    0x05, 0x01,  /* Usage Page (Generic Desktop) */
    0x09, 0x02,  /* Usage (Mouse) */
    0xA1, 0x01,  /* Collection (Application) */
    0x85, 0x02,  /*   Report ID (2) */
    0x09, 0x01,  /*   Usage (Pointer) */
    0xA1, 0x00,  /*   Collection (Physical) */
    /* Buttons */
    0x05, 0x09,  /*     Usage Page (Buttons) */
    0x19, 0x01,  /*     Usage Minimum (1) */
    0x29, 0x03,  /*     Usage Maximum (3) */
    0x15, 0x00,  /*     Logical Minimum (0) */
    0x25, 0x01,  /*     Logical Maximum (1) */
    0x75, 0x01,  /*     Report Size (1) */
    0x95, 0x03,  /*     Report Count (3) */
    0x81, 0x02,  /*     Input (Data, Variable, Absolute) */
    /* Padding */
    0x75, 0x05,  /*     Report Size (5) */
    0x95, 0x01,  /*     Report Count (1) */
    0x81, 0x01,  /*     Input (Constant) */
    /* X, Y, Wheel */
    0x05, 0x01,  /*     Usage Page (Generic Desktop) */
    0x09, 0x30,  /*     Usage (X) */
    0x09, 0x31,  /*     Usage (Y) */
    0x09, 0x38,  /*     Usage (Wheel) */
    0x15, 0x81,  /*     Logical Minimum (-127) */
    0x25, 0x7F,  /*     Logical Maximum (127) */
    0x75, 0x08,  /*     Report Size (8) */
    0x95, 0x03,  /*     Report Count (3) */
    0x81, 0x06,  /*     Input (Data, Variable, Relative) */
    0xC0,        /*   End Collection (Physical) */
    0xC0,        /* End Collection */

    /* ── Consumer Control (Report ID 3) ── */
    0x05, 0x0C,        /* Usage Page (Consumer) */
    0x09, 0x01,        /* Usage (Consumer Control) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x03,        /*   Report ID (3) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x26, 0xFF, 0x03,  /*   Logical Maximum (1023) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x03,  /*   Usage Maximum (1023) */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x00,        /*   Input (Data, Array, Absolute) */
    0xC0,              /* End Collection */
};

/* ─── HID Information characteristic value ───────────────────────── */
/* bcdHID=1.11, bCountryCode=0, Flags=0x03 (RemoteWake|NormConnect) */
static const uint8_t hid_info[] = { 0x11, 0x01, 0x00, 0x03 };

/* ─── Characteristic value store ────────────────────────────────── */
static uint8_t  g_proto_mode          = 0x01; /* Report Protocol */
static uint8_t  g_hid_ctrl_pt         = 0x00;
static uint8_t  g_battery_level       = 100;
static uint8_t  g_kbd_report[8]       = {0};
static uint8_t  g_mouse_report[4]     = {0};
static uint8_t  g_consumer_report[2]  = {0};
static volatile uint16_t g_consumer_report_handle = 0;

/* Report Reference descriptors (ID, type) */
static const uint8_t kbd_report_ref[]      = { 0x01, 0x01 }; /* ID=1, Input */
static const uint8_t mouse_report_ref[]    = { 0x02, 0x01 }; /* ID=2, Input */
static const uint8_t consumer_report_ref[] = { 0x03, 0x01 }; /* ID=3, Input */

/* ─── GATT read/write handlers ───────────────────────────────────── */

static int gatt_hid_info_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, hid_info, sizeof(hid_info)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_report_map_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, hid_report_map, sizeof(hid_report_map)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_ctrl_pt_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0) {
            os_mbuf_copydata(ctxt->om, 0, 1, &g_hid_ctrl_pt);
        }
    }
    return 0;
}

static int gatt_proto_mode_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &g_proto_mode, 1) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        os_mbuf_copydata(ctxt->om, 0, 1, &g_proto_mode);
    }
    return 0;
}

static int gatt_kbd_report_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, g_kbd_report, sizeof(g_kbd_report)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int gatt_kbd_report_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, kbd_report_ref, sizeof(kbd_report_ref)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_mouse_report_access(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, g_mouse_report, sizeof(g_mouse_report)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int gatt_mouse_report_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, mouse_report_ref, sizeof(mouse_report_ref)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_consumer_report_access(uint16_t conn_handle, uint16_t attr_handle,
                                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, g_consumer_report, sizeof(g_consumer_report)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int gatt_consumer_report_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, consumer_report_ref, sizeof(consumer_report_ref)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_battery_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, &g_battery_level, 1) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ─── GATT service table ─────────────────────────────────────────── */

static struct ble_gatt_chr_def hid_chars[] = {
    /* HID Information */
    {
        .uuid       = BLE_UUID16_DECLARE(0x2A4A),
        .access_cb  = gatt_hid_info_access,
        .flags      = BLE_GATT_CHR_F_READ,
    },
    /* Report Map */
    {
        .uuid       = BLE_UUID16_DECLARE(0x2A4B),
        .access_cb  = gatt_report_map_access,
        .flags      = BLE_GATT_CHR_F_READ,
    },
    /* HID Control Point */
    {
        .uuid       = BLE_UUID16_DECLARE(0x2A4C),
        .access_cb  = gatt_ctrl_pt_access,
        .flags      = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    /* Protocol Mode */
    {
        .uuid       = BLE_UUID16_DECLARE(0x2A4E),
        .access_cb  = gatt_proto_mode_access,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    /* Keyboard Input Report */
    {
        .uuid         = BLE_UUID16_DECLARE(0x2A4D),
        .access_cb    = gatt_kbd_report_access,
        .val_handle   = (uint16_t *)&g_kbd_report_handle,
        .flags        = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .descriptors  = (struct ble_gatt_dsc_def[]) {
            {
                /* Report Reference */
                .uuid      = BLE_UUID16_DECLARE(0x2908),
                .access_cb = gatt_kbd_report_ref_access,
                .att_flags = BLE_ATT_F_READ,
            },
            {0}, /* terminator */
        },
    },
    /* Mouse Input Report */
    {
        .uuid         = BLE_UUID16_DECLARE(0x2A4D),
        .access_cb    = gatt_mouse_report_access,
        .val_handle   = (uint16_t *)&g_mouse_report_handle,
        .flags        = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .descriptors  = (struct ble_gatt_dsc_def[]) {
            {
                /* Report Reference */
                .uuid      = BLE_UUID16_DECLARE(0x2908),
                .access_cb = gatt_mouse_report_ref_access,
                .att_flags = BLE_ATT_F_READ,
            },
            {0},
        },
    },
    /* Consumer Control Input Report */
    {
        .uuid         = BLE_UUID16_DECLARE(0x2A4D),
        .access_cb    = gatt_consumer_report_access,
        .val_handle   = (uint16_t *)&g_consumer_report_handle,
        .flags        = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .descriptors  = (struct ble_gatt_dsc_def[]) {
            {
                /* Report Reference */
                .uuid      = BLE_UUID16_DECLARE(0x2908),
                .access_cb = gatt_consumer_report_ref_access,
                .att_flags = BLE_ATT_F_READ,
            },
            {0},
        },
    },
    {0}, /* GATT characteristic terminator */
};

/* ─── Device Information Service (0x180A) ────────────────────────── */
static const uint8_t pnp_id[7] = {
    0x02,       /* Vendor ID Source: 0x02 = USB Implementers Forum */
    0xE5, 0x02, /* Vendor ID: 0x02E5 (Espressif) */
    0x01, 0xA1, /* Product ID: 0xA101 */
    0x00, 0x01  /* Product Version: 0x0100 (1.0.0) */
};
static const char manufacturer_name[] = "Espressif";
static const char model_number[]      = "ESP-HID-Bridge";

static int gatt_pnp_id_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, pnp_id, sizeof(pnp_id)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_mfg_name_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, manufacturer_name, strlen(manufacturer_name)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_model_num_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, model_number, strlen(model_number)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static struct ble_gatt_svc_def g_gatt_svcs[] = {
    /* Device Information Service (Required by Android HOGP) */
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = BLE_UUID16_DECLARE(0x2A50), /* PnP ID */
                .access_cb = gatt_pnp_id_access,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid      = BLE_UUID16_DECLARE(0x2A29), /* Manufacturer Name */
                .access_cb = gatt_mfg_name_access,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid      = BLE_UUID16_DECLARE(0x2A24), /* Model Number */
                .access_cb = gatt_model_num_access,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    /* Battery Service */
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = BLE_UUID16_DECLARE(0x2A19),
                .access_cb = gatt_battery_access,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    /* HID Service */
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(0x1812),
        .characteristics = hid_chars,
    },
    {0}, /* GATT service terminator */
};

/* ─── Advertising ────────────────────────────────────────────────── */

static uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;

static void start_advertising(void);

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected, handle=%d", g_conn_handle);

            /* Request minimum latency connection parameters */
            struct ble_gap_upd_params conn_params = {
                .itvl_min            = 6,    /* 7.5 ms */
                .itvl_max            = 12,   /* 15 ms */
                .latency             = 0,
                .supervision_timeout = 200,  /* 2 s */
                .min_ce_len          = 0,
                .max_ce_len          = 0,
            };
            ble_gap_update_params(g_conn_handle, &conn_params);
        } else {
            ESP_LOGI(TAG, "BLE connect failed, status=%d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d",
                 event->disconnect.reason);
        g_conn_handle        = BLE_HS_CONN_HANDLE_NONE;
        g_kbd_notify_en      = false;
        g_mouse_notify_en    = false;
        start_advertising();
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "BLE conn params updated");
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g_kbd_report_handle) {
            g_kbd_notify_en = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "Keyboard notify %s",
                     g_kbd_notify_en ? "enabled" : "disabled");
        } else if (event->subscribe.attr_handle == g_mouse_report_handle) {
            g_mouse_notify_en = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "Mouse notify %s",
                     g_mouse_notify_en ? "enabled" : "disabled");
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated to %d", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    /* Flags + Appearance (HID) + Name */
    struct ble_hs_adv_fields adv_fields = {0};
    adv_fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.appearance           = 0x03C0; /* HID: Generic HID */
    adv_fields.appearance_is_present = 1;
    adv_fields.name                 = (uint8_t *)"ESP-HID-Bridge";
    adv_fields.name_len             = strlen("ESP-HID-Bridge");
    adv_fields.name_is_complete     = 1;

    /* Advertise HID service UUID */
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    adv_fields.uuids16          = &hid_uuid;
    adv_fields.num_uuids16      = 1;
    adv_fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields error: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 100 ms advertising interval for fast discovery */
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(100);
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(100);

    rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start error: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started – name: ESP-HID-Bridge");
    }
}

/* ─── NimBLE host sync / reset callbacks ─────────────────────────── */

static void ble_on_sync(void)
{
    int rc;
    ESP_LOGI(TAG, "BLE host synced");

    /* Ensure we have a valid identity address (public preferred) */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_hs_util_ensure_addr returned %d, trying random", rc);
        rc = ble_hs_util_ensure_addr(1);
    }

    rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    rc = ble_hs_id_copy_addr(g_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE Device Address: %02X:%02X:%02X:%02X:%02X:%02X (type=%d)",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], g_own_addr_type);

    start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

/* ─── NimBLE host task ───────────────────────────────────────────── */

static void nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

#include "esp_hosted.h"
#include "esp_hosted_misc.h"

void ble_store_config_init(void);

esp_err_t ble_hid_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    /* Initialise connection to co-processor over SDIO */
    ESP_LOGI(TAG, "Connecting to ESP-Hosted slave (ESP32-C6)...");
    esp_hosted_connect_to_slave();

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble_port_init failed");

    /* Configure host callbacks */
    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    /* Secure pairing: Just Works (no passkey) */
    ble_hs_cfg.sm_io_cap    = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding   = 1;
    ble_hs_cfg.sm_mitm      = 0;
    ble_hs_cfg.sm_sc        = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Initialize persistent bonding storage */
    ble_store_config_init();

    /* Register GATT services */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(g_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(g_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* Set GAP device name */
    rc = ble_svc_gap_device_name_set("ESP-HID-Bridge");
    if (rc != 0) {
        ESP_LOGE(TAG, "gap device name set failed: %d", rc);
    }

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE HID device initialised");
    return ESP_OK;
}

esp_err_t ble_hid_send_keyboard(const uint8_t *report)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(g_kbd_report, report, KBD_REPORT_LEN);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, KBD_REPORT_LEN);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, g_kbd_report_handle, om);
    if (rc == 0 && report[2] != 0) {
        ESP_LOGI(TAG, "--> Sent key scancode 0x%02X to tablet via BLE!", report[2]);
    } else if (rc != 0) {
        ESP_LOGW(TAG, "ble_gatts_notify_custom kbd failed: %d", rc);
    }
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_hid_send_mouse(const uint8_t *report)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_mouse_notify_en) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(g_mouse_report, report, MOUSE_REPORT_LEN);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, MOUSE_REPORT_LEN);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, g_mouse_report_handle, om);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_hid_send_consumer(uint16_t usage)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    g_consumer_report[0] = (uint8_t)(usage & 0xFF);
    g_consumer_report[1] = (uint8_t)((usage >> 8) & 0xFF);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(g_consumer_report, sizeof(g_consumer_report));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, g_consumer_report_handle, om);
    if (rc == 0 && usage != 0) {
        ESP_LOGI(TAG, "--> Sent Consumer usage 0x%04X to tablet via BLE!", usage);
    } else if (rc != 0) {
        ESP_LOGW(TAG, "ble_gatts_notify_custom consumer failed: %d", rc);
    }
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

bool ble_hid_is_connected(void)
{
    return (g_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}
