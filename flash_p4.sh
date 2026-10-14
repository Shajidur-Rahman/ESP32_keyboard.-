#!/usr/bin/env bash
# ==============================================================================
# flash_p4.sh - One-Click Build & Safe Flasher for Waveshare ESP32-P4
# Compatible with ESP32-P4 v1.0 early silicon revisions
# ==============================================================================

set -e

# Detect Serial Port
PORT="${1:-/dev/ttyACM0}"
if [ ! -e "$PORT" ]; then
    # Fallback search for ports
    FOUND_PORT=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n 1 || true)
    if [ -n "$FOUND_PORT" ]; then
        PORT="$FOUND_PORT"
    fi
fi

echo "============================================================"
echo " 🚀 ESP32-P4 Safe One-Click Flasher & Launcher"
echo " Target Port: $PORT"
echo "============================================================"

# Ensure ESP-IDF environment is loaded
if [ -z "$IDF_PATH" ]; then
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        echo "📦 Sourcing ESP-IDF environment..."
        source "$HOME/esp/esp-idf/export.sh" > /dev/null 2>&1
    else
        echo "❌ Error: ESP-IDF not found at \$HOME/esp/esp-idf/export.sh"
        echo "Please set IDF_PATH or install ESP-IDF first."
        exit 1
    fi
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P4_DIR="$SCRIPT_DIR/p4_firmware"

if [ ! -d "$P4_DIR" ]; then
    echo "❌ Error: Cannot find p4_firmware directory at $P4_DIR"
    exit 1
fi

cd "$P4_DIR"

echo "🔨 Building firmware..."
idf.py build

echo ""
echo "⚡ Flashing to ESP32-P4 via esptool (Safe Mode for early silicon)..."
python3 -m esptool --chip esp32p4 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/usb_bt_bridge_p4.bin

echo ""
echo "🔄 Ensuring clean exit from ROM Bootloader into Application (boot:0x30f)..."
python3 -c "
import serial, time, sys
port = sys.argv[1]
try:
    s = serial.Serial(port, 115200, timeout=0.1)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    s.close()
    print('✅ Hardware reset line pulsed successfully.')
except Exception as e:
    print('⚠️ Note on reset pulse:', e)
" "$PORT"

echo ""
echo "============================================================"
echo " 🎉 Flashing Completed Successfully!"
echo " 1. Ensure the yellow jumper H3 is set to HOST (left pins)."
echo " 2. Pair your tablet to Bluetooth: 'ESP-HID-Bridge'."
echo " 3. Dedicated shortcuts: F5 (Play/Pause), F6 (Gemini Search)."
echo "============================================================"
