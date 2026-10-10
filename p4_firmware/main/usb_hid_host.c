/**
 * @file usb_hid_host.c
 * @brief USB HID Host driver for ESP32-P4 using raw USB Host Library.
 *
 * Implements a minimal HID class driver on top of the ESP-IDF USB Host
 * Library (usb_host.h), without depending on the external hid_host
 * component manager package.
 *
 * Supports simultaneous keyboard and mouse devices in Boot Protocol.
 *
 * ┌─────────────────────────────────────────────────────────────────┐
 * │ usb_lib_task  (Core 0, pri 5) – USB host library event loop     │
 * │ hid_class_task (Core 0, pri 4) – client event + interrupt pipe  │
 * └─────────────────────────────────────────────────────────────────┘
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

#include "usb_hid_host.h"
#include "bridge_protocol.h"

static const char *TAG = "usb_hid";

/* ─── USB HID class constants ─────────────────────────────────────── */
#define USB_CLASS_HID               0x03
#define HID_SUBCLASS_BOOT           0x01
#define HID_PROTO_KEYBOARD          0x01
#define HID_PROTO_MOUSE             0x02

/* HID class-specific requests */
#define HID_REQ_SET_PROTOCOL        0x0B
#define HID_REQ_SET_IDLE            0x0A

/* HID boot protocol = 0x00, report protocol = 0x01 */
#define HID_BOOT_PROTOCOL           0x00

/* Maximum HID interrupt IN transfer size */
#define HID_XFER_MAX_LEN            64
/* Maximum number of simultaneous HID devices tracked */
#define HID_DEV_MAX                 4

/* ─── Per-device state ────────────────────────────────────────────── */
typedef struct {
    bool                    in_use;
    usb_device_handle_t     dev_hdl;
    usb_transfer_t         *xfer;
    uint8_t                 dev_addr;
    uint8_t                 ep_addr;     /* Interrupt IN endpoint address */
    uint8_t                 proto;       /* HID_PROTO_KEYBOARD or HID_PROTO_MOUSE */
    uint8_t                 iface_num;
} hid_dev_t;

/* ─── Module state ────────────────────────────────────────────────── */
static QueueHandle_t     g_report_queue   = NULL;
static TaskHandle_t      g_usb_lib_task   = NULL;
static TaskHandle_t      g_hid_task       = NULL;
static SemaphoreHandle_t g_usb_ready      = NULL;
static usb_host_client_handle_t g_client_hdl = NULL;
static hid_dev_t         g_devs[HID_DEV_MAX];
static SemaphoreHandle_t g_devs_mutex     = NULL;

/* ─── Report parsers ─────────────────────────────────────────────── */

static void push_kbd(const uint8_t *data, size_t len)
{
    if (len < KBD_REPORT_LEN) return;
    bridge_pkt_t pkt = {
        .magic = BRIDGE_MAGIC, .type = BRIDGE_PKT_KEYBOARD,
        .len = KBD_REPORT_LEN, .reserved = 0
    };
    memcpy(pkt.data, data, KBD_REPORT_LEN);
    if (pkt.data[2] != 0) {
        ESP_LOGI(TAG, "USB Keyboard Event: key=0x%02X, mod=0x%02X", pkt.data[2], pkt.data[0]);
    }
    xQueueSend(g_report_queue, &pkt, 0);
}

static void push_mouse(const uint8_t *data, size_t len)
{
    if (len < MOUSE_REPORT_LEN) return;
    bridge_pkt_t pkt = {
        .magic = BRIDGE_MAGIC, .type = BRIDGE_PKT_MOUSE,
        .len = MOUSE_REPORT_LEN, .reserved = 0
    };
    memcpy(pkt.data, data, MOUSE_REPORT_LEN);
    xQueueSend(g_report_queue, &pkt, 0);
}

/* ─── Transfer callback ──────────────────────────────────────────── */

static void IRAM_ATTR hid_transfer_cb(usb_transfer_t *xfer)
{
    hid_dev_t *dev = (hid_dev_t *)xfer->context;

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        xfer->actual_num_bytes > 0) {
        if (dev->proto == HID_PROTO_KEYBOARD) {
            push_kbd(xfer->data_buffer, xfer->actual_num_bytes);
        } else {
            push_mouse(xfer->data_buffer, xfer->actual_num_bytes);
        }
    }

    /* Re-submit immediately for next report */
    if (xfer->status != USB_TRANSFER_STATUS_CANCELED &&
        xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        usb_host_transfer_submit(xfer);
    }
}

/* ─── HID setup helpers ──────────────────────────────────────────── */

static void IRAM_ATTR ctrl_transfer_cb(usb_transfer_t *xfer)
{
    usb_host_transfer_free(xfer);
}

/**
 * @brief Issue HID SET_PROTOCOL (boot=0) control transfer.
 */
static esp_err_t hid_set_boot_protocol(usb_device_handle_t dev_hdl,
                                        uint8_t iface_num)
{
    usb_transfer_t *ctrl_xfer = NULL;
    esp_err_t ret = usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ctrl_xfer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "alloc ctrl xfer failed: 0x%x", ret);
        return ret;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl_xfer->data_buffer;
    setup->bmRequestType = 0x21; /* Class | Interface | Host→Device */
    setup->bRequest      = HID_REQ_SET_PROTOCOL;
    setup->wValue        = HID_BOOT_PROTOCOL;
    setup->wIndex        = iface_num;
    setup->wLength       = 0;

    ctrl_xfer->device_handle    = dev_hdl;
    ctrl_xfer->bEndpointAddress = 0; /* EP0 */
    ctrl_xfer->num_bytes        = sizeof(usb_setup_packet_t);
    ctrl_xfer->timeout_ms       = 500;
    ctrl_xfer->callback         = ctrl_transfer_cb;
    ctrl_xfer->context          = NULL;

    ret = usb_host_transfer_submit_control(g_client_hdl, ctrl_xfer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SET_PROTOCOL submit failed: 0x%x", ret);
        usb_host_transfer_free(ctrl_xfer);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return ret;
}

/**
 * @brief Issue HID SET_IDLE (duration=0) control transfer.
 */
static esp_err_t hid_set_idle(usb_device_handle_t dev_hdl, uint8_t iface_num)
{
    usb_transfer_t *ctrl_xfer = NULL;
    esp_err_t ret = usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ctrl_xfer);
    if (ret != ESP_OK) {
        return ret;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl_xfer->data_buffer;
    setup->bmRequestType = 0x21; /* Class | Interface | Host→Device */
    setup->bRequest      = HID_REQ_SET_IDLE;
    setup->wValue        = 0;    /* 0 = infinite duration (report only on change) */
    setup->wIndex        = iface_num;
    setup->wLength       = 0;

    ctrl_xfer->device_handle    = dev_hdl;
    ctrl_xfer->bEndpointAddress = 0;
    ctrl_xfer->num_bytes        = sizeof(usb_setup_packet_t);
    ctrl_xfer->timeout_ms       = 500;
    ctrl_xfer->callback         = ctrl_transfer_cb;
    ctrl_xfer->context          = NULL;

    ret = usb_host_transfer_submit_control(g_client_hdl, ctrl_xfer);
    if (ret != ESP_OK) {
        usb_host_transfer_free(ctrl_xfer);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return ret;
}


/* ─── Device enumeration ─────────────────────────────────────────── */

static void handle_new_device(uint8_t dev_addr)
{
    usb_device_handle_t dev_hdl;
    esp_err_t ret;

    ret = usb_host_device_open(g_client_hdl, dev_addr, &dev_hdl);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "device_open(%u) failed: 0x%x", dev_addr, ret);
        return;
    }

    const usb_config_desc_t *config_desc;
    ret = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (ret != ESP_OK) {
        usb_host_device_close(g_client_hdl, dev_hdl);
        return;
    }

    /* Walk interface descriptors looking for HID Boot keyboards/mice */
    int offset = 0;
    int total_len = config_desc->wTotalLength;

    while (offset < total_len) {
        const usb_standard_desc_t *desc =
            (const usb_standard_desc_t *)((uint8_t *)config_desc + offset);

        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;

            /* HID device: Keyboard or Mouse */
            if (intf->bInterfaceClass == USB_CLASS_HID) {
                uint8_t proto = intf->bInterfaceProtocol;
                if (proto == 0) {
                    /* Default to Keyboard if protocol is unspecified */
                    proto = HID_PROTO_KEYBOARD;
                }
                if (proto != HID_PROTO_KEYBOARD && proto != HID_PROTO_MOUSE) {
                    if (desc->bLength == 0) break;
                    offset += desc->bLength;
                    continue;
                }

                ESP_LOGI(TAG, "Found HID %s at iface=%u (subclass=%u, proto=%u)",
                         proto == HID_PROTO_KEYBOARD ? "Keyboard" : "Mouse",
                         intf->bInterfaceNumber,
                         intf->bInterfaceSubClass,
                         intf->bInterfaceProtocol);

                /* Find the Interrupt IN endpoint in this interface */
                int ep_offset = offset + intf->bLength;
                uint8_t ep_addr = 0;
                uint16_t ep_mps = 0;

                while (ep_offset < total_len) {
                    const usb_standard_desc_t *ep_desc_gen =
                        (const usb_standard_desc_t *)((uint8_t *)config_desc + ep_offset);

                    if (ep_desc_gen->bLength == 0) break;
                    if (ep_desc_gen->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
                        break; /* Reached next interface */
                    }
                    if (ep_desc_gen->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
                        const usb_ep_desc_t *ep = (const usb_ep_desc_t *)ep_desc_gen;
                        bool is_in  = (ep->bEndpointAddress & 0x80) != 0;
                        uint8_t type = ep->bmAttributes & 0x03;
                        if (is_in && type == 0x03) { /* Interrupt IN */
                            ep_addr = ep->bEndpointAddress;
                            ep_mps  = ep->wMaxPacketSize;
                            ESP_LOGI(TAG, "  Found EP 0x%02X (MPS=%u) for iface %u",
                                     ep_addr, ep_mps, intf->bInterfaceNumber);
                            break;
                        }
                    }
                    ep_offset += ep_desc_gen->bLength;
                }

                if (ep_addr == 0) {
                    ESP_LOGW(TAG, "No interrupt IN ep found in iface %u",
                             intf->bInterfaceNumber);
                } else {
                    /* Find a free slot */
                    xSemaphoreTake(g_devs_mutex, portMAX_DELAY);
                    hid_dev_t *slot = NULL;
                    for (int j = 0; j < HID_DEV_MAX; j++) {
                        if (!g_devs[j].in_use) { slot = &g_devs[j]; break; }
                    }

                    if (!slot) {
                        ESP_LOGW(TAG, "No free HID device slot");
                        xSemaphoreGive(g_devs_mutex);
                    } else {
                        /* Claim the interface */
                        ret = usb_host_interface_claim(g_client_hdl, dev_hdl,
                                                       intf->bInterfaceNumber, 0);
                        if (ret != ESP_OK) {
                            ESP_LOGW(TAG, "iface_claim failed: 0x%x", ret);
                            xSemaphoreGive(g_devs_mutex);
                        } else {
                            /* Allocate interrupt IN transfer */
                            usb_transfer_t *xfer = NULL;
                            uint16_t xfer_size = (ep_mps > 0) ? ep_mps : HID_XFER_MAX_LEN;
                            ret = usb_host_transfer_alloc(xfer_size, 0, &xfer);
                            if (ret != ESP_OK) {
                                ESP_LOGW(TAG, "transfer_alloc failed");
                                usb_host_interface_release(g_client_hdl, dev_hdl,
                                                           intf->bInterfaceNumber);
                                xSemaphoreGive(g_devs_mutex);
                            } else {
                                slot->in_use    = true;
                                slot->dev_hdl   = dev_hdl;
                                slot->dev_addr  = dev_addr;
                                slot->ep_addr   = ep_addr;
                                slot->proto     = proto;
                                slot->iface_num = intf->bInterfaceNumber;
                                slot->xfer      = xfer;

                                xfer->device_handle     = dev_hdl;
                                xfer->bEndpointAddress  = ep_addr;
                                xfer->callback          = hid_transfer_cb;
                                xfer->context           = slot;
                                xfer->num_bytes         = xfer_size;
                                xfer->timeout_ms        = 0; /* No timeout for IN polling */

                                /* Set Boot Protocol and Idle Rate */
                                hid_set_boot_protocol(dev_hdl, intf->bInterfaceNumber);
                                if (proto == HID_PROTO_KEYBOARD) {
                                    hid_set_idle(dev_hdl, intf->bInterfaceNumber);
                                }

                                ret = usb_host_transfer_submit(xfer);
                                if (ret != ESP_OK) {
                                    ESP_LOGW(TAG, "transfer_submit failed: 0x%x", ret);
                                    slot->in_use = false;
                                    usb_host_transfer_free(xfer);
                                    usb_host_interface_release(g_client_hdl, dev_hdl,
                                                               intf->bInterfaceNumber);
                                }
                            }
                        }
                    }
                    xSemaphoreGive(g_devs_mutex);
                }
            }
        }

        if (desc->bLength == 0) break; /* Safety: avoid infinite loop */
        offset += desc->bLength;
    }
}

static void handle_device_gone(usb_device_handle_t dev_hdl)
{
    xSemaphoreTake(g_devs_mutex, portMAX_DELAY);
    for (int i = 0; i < HID_DEV_MAX; i++) {
        if (g_devs[i].in_use && g_devs[i].dev_hdl == dev_hdl) {
            usb_host_transfer_free(g_devs[i].xfer);
            usb_host_interface_release(g_client_hdl, g_devs[i].dev_hdl,
                                       g_devs[i].iface_num);
            usb_host_device_close(g_client_hdl, g_devs[i].dev_hdl);
            memset(&g_devs[i], 0, sizeof(hid_dev_t));
            ESP_LOGI(TAG, "HID device %p removed", dev_hdl);
        }
    }
    xSemaphoreGive(g_devs_mutex);
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "New USB device: addr=%u", event_msg->new_dev.address);
        handle_new_device(event_msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "USB device gone: hdl=%p", event_msg->dev_gone.dev_hdl);
        handle_device_gone(event_msg->dev_gone.dev_hdl);
        break;
    default:
        break;
    }
}

static bool usb_enum_filter_cb(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue)
{
    ESP_LOGI(TAG, "USB Filter: Found VID=0x%04X PID=0x%04X, allowing enumeration",
             dev_desc->idVendor, dev_desc->idProduct);
    *bConfigurationValue = 1;
    return true;
}

/* ─── USB Host library task ──────────────────────────────────────── */

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = usb_enum_filter_cb,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xSemaphoreGive(g_usb_ready);

    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            break;
        }
    }
    usb_host_uninstall();
    vTaskDelete(NULL);
}

/* ─── HID client task ────────────────────────────────────────────── */

static void hid_class_task(void *arg)
{
    xSemaphoreTake(g_usb_ready, portMAX_DELAY);

    const usb_host_client_config_t client_cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 10,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg          = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_cfg, &g_client_hdl));
    ESP_LOGI(TAG, "USB HID client registered – waiting for devices");

    while (1) {
        usb_host_client_handle_events(g_client_hdl, portMAX_DELAY);
    }

    usb_host_client_deregister(g_client_hdl);
    vTaskDelete(NULL);
}

/* ─── Public API ─────────────────────────────────────────────────── */

esp_err_t usb_hid_host_start(QueueHandle_t report_queue)
{
    ESP_RETURN_ON_FALSE(report_queue, ESP_ERR_INVALID_ARG, TAG,
                        "report_queue is NULL");

    g_report_queue = report_queue;
    memset(g_devs, 0, sizeof(g_devs));

    g_devs_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_devs_mutex, ESP_ERR_NO_MEM, TAG, "mutex alloc");

    g_usb_ready = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(g_usb_ready, ESP_ERR_NO_MEM, TAG, "sem alloc");

    BaseType_t r;
    r = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 5,
                                &g_usb_lib_task, 0);
    ESP_RETURN_ON_FALSE(r == pdPASS, ESP_FAIL, TAG, "usb_lib_task create");

    r = xTaskCreatePinnedToCore(hid_class_task, "hid_class", 6144, NULL, 4,
                                &g_hid_task, 0);
    ESP_RETURN_ON_FALSE(r == pdPASS, ESP_FAIL, TAG, "hid_class_task create");

    ESP_LOGI(TAG, "USB HID host subsystem started");
    return ESP_OK;
}

void usb_hid_host_stop(void)
{
    if (g_hid_task)    { vTaskDelete(g_hid_task);    g_hid_task    = NULL; }
    if (g_usb_lib_task){ vTaskDelete(g_usb_lib_task); g_usb_lib_task= NULL; }
    if (g_devs_mutex)  { vSemaphoreDelete(g_devs_mutex); g_devs_mutex = NULL; }
    if (g_usb_ready)   { vSemaphoreDelete(g_usb_ready);  g_usb_ready  = NULL; }
}
