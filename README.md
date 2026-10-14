# ⌨️ ESP32-P4 Wireless Keyboard & Mouse Bridge

> Turn any wired USB Type-A keyboard and mouse into a high-performance, low-latency wireless Bluetooth & Wi-Fi device using the **Waveshare ESP32-P4** board.

Designed specifically for tablets (**Redmi Pad SE / Android**), phones, and PCs without requiring any wires to your device.

---

## ✨ Features

- 📶 **100% Wireless BLE HID (HOGP):** Pairs directly as `ESP-HID-Bridge` (composite Keyboard + Mouse).
- ⚡ **Zero-Lag Input:** Responsive HID polling with instant event propagation.
- ⏯️ **Dedicated Media & AI Shortcuts:**
  - **`F5`** ➡️ **Global Media Play / Pause** (instantly controls videos in Telegram, Chrome, YouTube, Spotify).
  - **`F6`** ➡️ **Gemini / Circle to Search** (launches Google / Gemini screen search overlay).
  - Normal keys and modifier combos (`Ctrl+F5`, `Shift+F5`) remain untouched for normal typing.
- 🚀 **Wi-Fi 6 UDP Streaming:** Optional ultra-low latency (<1 ms) streaming to laptops via Python receiver.
- 🔋 **Ultra-Low Power:** Adds `< 1%` daily battery drain on tablets and can run 35–40+ hours on a standard 10,000 mAh power bank.

---

## 🛠️ Hardware Setup

### 1. Crucial OTG Jumper (`H3`)
The board uses an onboard high-speed USB multiplexer (`U15` FSUSB42) to route the data lines ($D+/D-$) to either the USB-C OTG port or the 4 USB Type-A ports.

Set the yellow jumper header **`H3`** to **`HOST`**:

```
        ┌───────────────────────────────┐
        │   HOST      [ • • ]   (Left)  │  <-- Jumper MUST bridge these 2 pins
        │   DEVICE      [ • ]   (Right) │  <-- Leave rightmost pin exposed
        └───────────────────────────────┘
```
*(If left on `DEVICE`, the ports will supply 5V power but USB data lines will remain physically disconnected).*

### 2. Cable Connections
- **Keyboard / Mouse:** Plug into any of the 4 USB Type-A sockets.
- **Computer / Power:** Connect your USB-C cable to the port labeled **`PWR USB TO UART`** on the bottom of the board.

---

## ⚡ One-Command Flash (Safe for Early Silicon Revisions)

Early silicon revisions (e.g. ESP32-P4 v1.0) can enter a bootloader loop (`boot:0x307 waiting for download`) if reset signals are mistimed. We have provided an automated, safe one-click flash script:

```bash
./flash_p4.sh
```

*(Optional: specify your port if not on `/dev/ttyACM0`)*:
```bash
./flash_p4.sh /dev/ttyACM0
```

### What this script does automatically:
1. Sources the ESP-IDF environment cleanly.
2. Compiles the firmware with custom descriptor parsers and USB Host Hub support.
3. Flashes the bootloader, partition table, and app with the safe `460800` baud rate and `dio` flash parameters.
4. Pulses the hardware reset line to cleanly transition from ROM download mode directly into application execution (`boot:0x30f SPI_FAST_FLASH_BOOT`).

---

## 📦 Manual Installation (Step-by-Step)

If you prefer building and flashing manually:

### 1. Prerequisites
Install and activate ESP-IDF (v5.3+ or v5.5):
```bash
source $HOME/esp/esp-idf/export.sh
```

Ensure your user account has serial port permissions:
```bash
sudo usermod -a -G dialout $USER
```

### 2. Build the Project
```bash
cd p4_firmware
idf.py build
```

### 3. Flash to ESP32-P4
```bash
python3 -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/usb_bt_bridge_p4.bin
```

### 4. If Board Stays in "Waiting for Download"
Simply press the physical **RST** button on the board, or run:
```bash
python3 -c "import serial, time; s = serial.Serial('/dev/ttyACM0', 115200); s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False; s.close()"
```

---

## 📱 Connecting to Redmi Pad SE / Android / PC

1. Open **Settings → Bluetooth** on your tablet.
2. Select **`ESP-HID-Bridge`**.
3. Tap **Pair / Connect** (uses standard "Just Works" encryption, no PIN required).
4. Start typing on your USB keyboard!

### Real-Time Serial Monitor:
To inspect keyboard events and connection states live:
```bash
idf.py -p /dev/ttyACM0 monitor
```

---

## 💻 Optional: Laptop UDP Streaming

To stream inputs to a Linux/Windows/Mac PC over local Wi-Fi:
1. Edit Wi-Fi credentials in `p4_firmware/main/wifi_stream.c`.
2. Run the receiver on your laptop:
   ```bash
   python3 laptop_receiver.py
   ```
