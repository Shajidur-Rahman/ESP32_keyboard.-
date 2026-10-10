#!/usr/bin/env python3
"""
Low-Latency Input Receiver for ESP32-P4 + C6 Wireless Bridge.
Listens on UDP port 5000 and injects HID keyboard/mouse events into Linux uinput.

Requirements:
    pip install evdev
    (or run with sudo python3 laptop_receiver.py)
"""

import socket
import struct
import sys
import os

try:
    import evdev
    from evdev import UInput, ecodes as e
    HAS_EVDEV = True
except ImportError:
    HAS_EVDEV = False

PORT = 5000
PKT_FMT = "<BBH4s8s"
# struct bridge_pkt {
#     uint8_t type;         // 1 = KBD, 2 = MOUSE
#     uint8_t length;
#     uint16_t reserved;
#     uint8_t header[4];
#     uint8_t report[8];
# }

# USB HID Boot Keyboard Keycode to Linux evdev mapping table
HID_TO_EVDEV = {
    0x04: e.KEY_A if HAS_EVDEV else 0,
    0x05: e.KEY_B if HAS_EVDEV else 0,
    0x06: e.KEY_C if HAS_EVDEV else 0,
    0x07: e.KEY_D if HAS_EVDEV else 0,
    0x08: e.KEY_E if HAS_EVDEV else 0,
    0x09: e.KEY_F if HAS_EVDEV else 0,
    0x0A: e.KEY_G if HAS_EVDEV else 0,
    0x0B: e.KEY_H if HAS_EVDEV else 0,
    0x0C: e.KEY_I if HAS_EVDEV else 0,
    0x0D: e.KEY_J if HAS_EVDEV else 0,
    0x0E: e.KEY_K if HAS_EVDEV else 0,
    0x0F: e.KEY_L if HAS_EVDEV else 0,
    0x10: e.KEY_M if HAS_EVDEV else 0,
    0x11: e.KEY_N if HAS_EVDEV else 0,
    0x12: e.KEY_O if HAS_EVDEV else 0,
    0x13: e.KEY_P if HAS_EVDEV else 0,
    0x14: e.KEY_Q if HAS_EVDEV else 0,
    0x15: e.KEY_R if HAS_EVDEV else 0,
    0x16: e.KEY_S if HAS_EVDEV else 0,
    0x17: e.KEY_T if HAS_EVDEV else 0,
    0x18: e.KEY_U if HAS_EVDEV else 0,
    0x19: e.KEY_V if HAS_EVDEV else 0,
    0x1A: e.KEY_W if HAS_EVDEV else 0,
    0x1B: e.KEY_X if HAS_EVDEV else 0,
    0x1C: e.KEY_Y if HAS_EVDEV else 0,
    0x1D: e.KEY_Z if HAS_EVDEV else 0,
    0x1E: e.KEY_1 if HAS_EVDEV else 0,
    0x1F: e.KEY_2 if HAS_EVDEV else 0,
    0x20: e.KEY_3 if HAS_EVDEV else 0,
    0x21: e.KEY_4 if HAS_EVDEV else 0,
    0x22: e.KEY_5 if HAS_EVDEV else 0,
    0x23: e.KEY_6 if HAS_EVDEV else 0,
    0x24: e.KEY_7 if HAS_EVDEV else 0,
    0x25: e.KEY_8 if HAS_EVDEV else 0,
    0x26: e.KEY_9 if HAS_EVDEV else 0,
    0x27: e.KEY_0 if HAS_EVDEV else 0,
    0x28: e.KEY_ENTER if HAS_EVDEV else 0,
    0x29: e.KEY_ESC if HAS_EVDEV else 0,
    0x2A: e.KEY_BACKSPACE if HAS_EVDEV else 0,
    0x2B: e.KEY_TAB if HAS_EVDEV else 0,
    0x2C: e.KEY_SPACE if HAS_EVDEV else 0,
    0x2D: e.KEY_MINUS if HAS_EVDEV else 0,
    0x2E: e.KEY_EQUAL if HAS_EVDEV else 0,
    0x2F: e.KEY_LEFTBRACE if HAS_EVDEV else 0,
    0x30: e.KEY_RIGHTBRACE if HAS_EVDEV else 0,
    0x31: e.KEY_BACKSLASH if HAS_EVDEV else 0,
    0x33: e.KEY_SEMICOLON if HAS_EVDEV else 0,
    0x34: e.KEY_APOSTROPHE if HAS_EVDEV else 0,
    0x35: e.KEY_GRAVE if HAS_EVDEV else 0,
    0x36: e.KEY_COMMA if HAS_EVDEV else 0,
    0x37: e.KEY_DOT if HAS_EVDEV else 0,
    0x38: e.KEY_SLASH if HAS_EVDEV else 0,
    0x39: e.KEY_CAPSLOCK if HAS_EVDEV else 0,
    0x4F: e.KEY_RIGHT if HAS_EVDEV else 0,
    0x50: e.KEY_LEFT if HAS_EVDEV else 0,
    0x51: e.KEY_DOWN if HAS_EVDEV else 0,
    0x52: e.KEY_UP if HAS_EVDEV else 0,
}

MODIFIERS = [
    (0x01, e.KEY_LEFTCTRL if HAS_EVDEV else 0),
    (0x02, e.KEY_LEFTSHIFT if HAS_EVDEV else 0),
    (0x04, e.KEY_LEFTALT if HAS_EVDEV else 0),
    (0x08, e.KEY_LEFTMETA if HAS_EVDEV else 0),
    (0x10, e.KEY_RIGHTCTRL if HAS_EVDEV else 0),
    (0x20, e.KEY_RIGHTSHIFT if HAS_EVDEV else 0),
    (0x40, e.KEY_RIGHTALT if HAS_EVDEV else 0),
    (0x80, e.KEY_RIGHTMETA if HAS_EVDEV else 0),
]

def setup_uinput():
    if not HAS_EVDEV:
        return None
    try:
        cap = {
            e.EV_KEY: list(HID_TO_EVDEV.values()) + [m[1] for m in MODIFIERS] + [
                e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE
            ],
            e.EV_REL: [e.REL_X, e.REL_Y, e.REL_WHEEL],
        }
        return UInput(cap, name="ESP32-P4 Wireless Bridge")
    except Exception as ex:
        print(f"[!] Warning: Cannot open /dev/uinput ({ex}). Running in packet inspection mode only.")
        return None

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", PORT))
    print(f"[*] ESP32-P4 Wireless Input Bridge receiver listening on UDP port {PORT}...")

    ui = setup_uinput()
    if ui:
        print("[*] Virtual device created via uinput. Forwarded inputs will take effect on your system!")
    else:
        print("[*] Operating in Monitor Mode (printing incoming packets to terminal).")

    prev_pressed_keys = set()
    prev_buttons = 0

    while True:
        data, addr = sock.recvfrom(64)
        if len(data) < 16:
            continue
        pkt_type, pkt_len, _, _, report = struct.unpack(PKT_FMT, data[:16])

        if pkt_type == 1:
            # Keyboard Report (modifier, reserved, keycodes[6])
            modifier = report[0]
            keys = set(k for k in report[2:8] if k != 0)

            if ui:
                # Modifiers
                for bit, keycode in MODIFIERS:
                    ui.write(e.EV_KEY, keycode, 1 if (modifier & bit) else 0)

                # Pressed keys
                new_keys = keys - prev_pressed_keys
                released_keys = prev_pressed_keys - keys

                for k in released_keys:
                    if k in HID_TO_EVDEV:
                        ui.write(e.EV_KEY, HID_TO_EVDEV[k], 0)
                for k in new_keys:
                    if k in HID_TO_EVDEV:
                        ui.write(e.EV_KEY, HID_TO_EVDEV[k], 1)
                ui.syn()
                prev_pressed_keys = keys
            else:
                print(f"[KBD] from {addr[0]}: Mod=0x{modifier:02X} Keys={[hex(k) for k in keys]}")

        elif pkt_type == 2:
            # Mouse Report (buttons, dx, dy, wheel)
            buttons = report[0]
            dx = struct.unpack("b", bytes([report[1]]))[0]
            dy = struct.unpack("b", bytes([report[2]]))[0]
            wheel = struct.unpack("b", bytes([report[3]]))[0] if pkt_len > 3 else 0

            if ui:
                ui.write(e.EV_REL, e.REL_X, dx)
                ui.write(e.EV_REL, e.REL_Y, dy)
                if wheel != 0:
                    ui.write(e.EV_REL, e.REL_WHEEL, wheel)
                ui.write(e.EV_KEY, e.BTN_LEFT, 1 if (buttons & 0x01) else 0)
                ui.write(e.EV_KEY, e.BTN_RIGHT, 1 if (buttons & 0x02) else 0)
                ui.write(e.EV_KEY, e.BTN_MIDDLE, 1 if (buttons & 0x04) else 0)
                ui.syn()
            else:
                print(f"[MOUSE] from {addr[0]}: Buttons=0x{buttons:02X} dx={dx} dy={dy} wheel={wheel}")

if __name__ == "__main__":
    main()
