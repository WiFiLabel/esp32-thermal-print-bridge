# Waveshare ESP32-P4-NANO cheat sheet

Official docs: https://docs.waveshare.com/ESP32-P4-NANO

## Ports used by this firmware

| Port / part | Role |
| --- | --- |
| **USB-C** | Power, flash, serial monitor (USB-UART bridge) |
| **USB-A Type-A** | USB 2.0 OTG High Speed **host** to thermal printer |
| **RJ45** | 100 Mbps Ethernet (prefer for first bring-up) |
| **BOOT** | Hold during reset/power-up to enter download mode |
| **RESET** | Hard reset |
| **ESP32-C6-MINI-1** | Wi-Fi 6 / BLE via SDIO (`esp_hosted` / `esp_wifi_remote`) |

## Unused in v1

MIPI-DSI display, MIPI-CSI camera, speaker / mic, TF card, GPIO headers, PoE module header, RTC battery header.

## Memory / storage

- ESP32-P4NRW32 with **32 MB** stacked PSRAM
- Onboard **16 MB** NOR flash
- Dual OTA: see `firmware/partitions.csv`

## Flash / monitor (macOS)

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32p4
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

List candidates: `ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null`
