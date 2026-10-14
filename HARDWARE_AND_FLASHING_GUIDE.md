# ESP32-P4 Hardware Specifications & Early Silicon Flashing Guide

---

## 📌 Complete Hardware & Board Identification

This project is built and optimized for the **Waveshare ESP32-P4-Module-DEV-KIT** (specifically with **ESP32-P4 v1.0 early silicon**).

### 🔬 Chip & Module Specifications
- **Board Name:** Waveshare ESP32-P4-Module-DEV-KIT
- **SoC Model:** Espressif **ESP32-P4**
- **Chip Revision:** **v1.0 (ECO2 - Early Silicon)**
  - *ROM Version:* `ESP-ROM:esp32p4-eco2-20240710`
- **CPU Architecture:**
  - **HP (High-Performance) Core:** Dual-Core 32-bit RISC-V (`RV32IMAFCP`) clocked up to **400 MHz** (running @ 360 MHz).
  - **LP (Low-Power) Core:** Single-Core RISC-V clocked @ **40 MHz**.
- **Memory Configuration:**
  - **Internal SRAM:** 768 KB L2 SRAM + 32 KB LP SRAM + 128 KB TCM.
  - **External PSRAM:** **32 MB (256 Mbit)** Ultra-High-Speed Hexa-SRAM (`APVendor gen4`, 16-bit wide bus @ 80 MHz, fixed latency).
  - **Flash Storage:** **16 MB** Quad-SPI (GD flash chip).
- **Wireless Co-Processor:**
  - Onboard **ESP32-C6** connected to the P4 over an internal **4-bit high-speed SDIO bus (40 MHz)** for:
    - **Wi-Fi 6 (802.11ax)** 2.4 GHz
    - **Bluetooth 5.3 (LE)** & Bluetooth Mesh

### 🔌 Board Peripherals & USB Topology
- **USB Controller:** High-Speed USB 2.0 OTG Host/Device with dedicated on-chip PHY.
- **Onboard 4-Port USB Hub:** **WCH CH334F** 4-port USB 2.0 Hub connected to 4x USB Type-A ports.
- **USB Multiplexer:** **FSUSB42UMX (`U15`)** controlled by 3-pin jumper **`H3`**:
  - `HOST` mode: Routes D+/D- lines to the 4 Type-A ports (CH334F hub).
  - `DEVICE` mode: Routes D+/D- lines to the bottom USB-C OTG port.
- **Console / Flashing Interface:** **CH343P** USB-to-UART connected to port labeled **`PWR USB TO UART`**.

---

## ⚠️ Why ESP32-P4 v1.0 (Early Silicon) Needs Special Care

The **ESP32-P4 v1.0 / ECO2** has unique silicon and bootloader behaviors that can cause flashing to fail or brick the boot loop if using standard generic flashing commands:

1. **Auto-Reset Timing Issue (`waiting for download / boot:0x307`):**
   - Standard tools like `idf.py flash` or vanilla `esptool.py` toggle DTR/RTS in a way that leaves the early P4 silicon trapped in ROM Download Mode (`waiting for download`).
   - The chip will not boot your application until a specific sequence (DTR=False, RTS pulse) is applied.
2. **Flash Speed & Mode Constraints:**
   - Must be flashed with **`--flash_mode dio`** at **`--flash_freq 40m`** and **`--flash_size 16MB`**. Using `qio` during flashing can corrupt flash reads on early bootloader stages.
3. **Partition Offsets for P4 Bootloader:**
   - Bootloader: `0x2000` (Unlike classic ESP32 which is at `0x1000`).
   - Partition Table: `0x8000`.
   - Application binary: `0x10000`.
4. **USB Host Enumeration Filter Bug in ESP-IDF:**
   - Early ESP-IDF configurations enable `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y`. Without an explicit callback in code, early silicon aborts enumeration on all connected USB devices (`Abort request of enumeration process`). Our firmware code includes an explicit callback to resolve this.

---

## 🚀 How to Flash Without Errors (Safe Methods)

### Method 1: The Automated One-Click Script (Recommended)
We provided a turnkey script in the repository that handles build, partition flashing, baud rates, and post-flash reset:

```bash
./flash_p4.sh
```

*(If on a different serial port)*:
```bash
./flash_p4.sh /dev/ttyACM0
```

---

### Method 2: Manual Step-by-Step Command

If you want to run `esptool` manually:

```bash
# 1. Activate your ESP-IDF environment
source $HOME/esp/esp-idf/export.sh

# 2. Compile
cd p4_firmware
idf.py build

# 3. Flash all partitions safely
python3 -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/usb_bt_bridge_p4.bin
```

---

### 🛠️ Troubleshooting: Chip Stuck in "waiting for download"?

If your serial monitor shows:
```text
rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))
waiting for download
```
The early silicon is waiting for an upload packet. **Do not re-flash!** Just release the reset line into SPI Flash boot mode:

**Option A (Terminal Command):**
```bash
python3 -c "import serial, time; s = serial.Serial('/dev/ttyACM0', 115200); s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False; s.close()"
```

**Option B (Hardware):**
Press the physical **RST** button once on the board.

The terminal will immediately confirm:
```text
rst:0x1 (POWERON),boot:0x30f (SPI_FAST_FLASH_BOOT)
...
I ble_hid: BLE advertising started – name: ESP-HID-Bridge
```
