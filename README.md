# ESP32-P4 + ESP32-C6 USB-to-Wireless (Bluetooth & Wi-Fi) HID Bridge

Connect any standard wired USB Type-A keyboard or mouse to the **Waveshare ESP32-P4** board and stream inputs wirelessly with ultra-low latency:
1. **Bluetooth LE HID (HOGP)** directly to tablets (e.g. Android / Redmi Pad SE), phones, and PCs — with **zero physical wires**.
2. **Wi-Fi 6 (UDP broadcast)** for <1 ms latency streaming to laptops/PCs running `laptop_receiver.py`.

---

## Hardware Configuration & Jumper Settings

### 1. Crucial USB OTG Jumper (`H3`)
The Waveshare ESP32-P4 board contains an onboard high-speed USB multiplexer switch (`U15` FSUSB42) controlling data lines to the 4 USB Type-A ports:

```
        ┌──────────────┐
        │  HOST   [••] │   <-- Yellow jumper MUST bridge the HOST pins (Left)
        │  DEVICE  [ ] │   <-- Exposed pin on right
        └──────────────┘
```

* **Must be in `HOST` mode:** Ensure the yellow jumper bridges the pins towards the word **`HOST`**.
* *If the jumper is in `DEVICE` mode, 5V power will be present, but USB data lines ($D+/D-$) are physically disconnected from the 4 Type-A ports!*

### 2. Cable Connections
* **Keyboard / Mouse:** Plug into any of the 4 USB Type-A ports.
* **Flashing / Debug Port:** Connect USB-C cable to the port labeled **`PWR USB TO UART`** on the bottom of the board.

---

## How to Flash Without Errors

### 1. Prerequisites
Activate ESP-IDF (v5.3+ or v5.5 recommended):
```bash
source $HOME/esp/esp-idf/export.sh
```

Ensure your user has access to serial ports (`/dev/ttyACM0` or `/dev/ttyUSB0`):
```bash
sudo usermod -a -G dialout $USER
```

---

### 2. Building the Firmware
Navigate to the P4 firmware directory and compile:
```bash
cd p4_firmware
idf.py build
```

---

### 3. Flashing Using `esptool` (Foolproof Method)

Due to early silicon USB-UART auto-reset timing, flashing directly with `python -m esptool` is the most reliable and error-free method:

#### Complete Initial Flash (Bootloader + Partition Table + App):
```bash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/usb_bt_bridge_p4.bin
```

#### Fast App-Only Update (After code edits):
```bash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  0x10000 build/usb_bt_bridge_p4.bin
```

---

### 4. Resolving "Waiting for Download" / Boot Stuck Error

If the ESP32-P4 gets stuck in ROM download mode after flashing (`rst:0x1, boot:0x307 (DOWNLOAD)`):
1. **Hardware Reset:** Press the **RST** button on the board once.
2. **Software Reset via Python:**
   ```bash
   python3 -c "import serial, time; s = serial.Serial('/dev/ttyACM0', 115200); s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False; s.close()"
   ```
The chip will immediately boot into normal SPI Flash execution (`boot:0x30f (SPI_FAST_FLASH_BOOT)`).

---

## Connecting to Your Tablet / PC

1. **Bluetooth Pairing:**
   - On your tablet/PC, open **Settings → Bluetooth**.
   - Select **`ESP-HID-Bridge`**.
   - Tap **Pair / Connect**. No PIN or passkey is needed ("Just Works" security).
   - Once connected, both keyboard and mouse inputs are active immediately.

2. **Monitoring Logs in Real-Time:**
   ```bash
   idf.py -p /dev/ttyACM0 monitor
   ```
   You will see:
   ```text
   I usb_hid: Found HID Keyboard at iface=0 (subclass=1, proto=1)
   I usb_hid: Found EP 0x81 (MPS=8) for iface 0
   I ble_hid: BLE connected, handle=0
   I usb_hid: USB Keyboard Event: key=0x04, mod=0x00
   I ble_hid: --> Sent key scancode 0x04 to tablet via BLE!
   ```

---

## Wi-Fi UDP Streaming (Optional PC Receiver)

For PC/laptop usage over local Wi-Fi with <1 ms latency:
1. Configure Wi-Fi SSID and password in `p4_firmware/main/wifi_stream.c`.
2. On your laptop, run:
   ```bash
   python3 laptop_receiver.py
   ```
3. Keystrokes and mouse movements will be injected directly into the operating system at hardware-like speeds.
