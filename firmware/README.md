# firmware

ESP-IDF firmware for the Waveshare **ESP32-P4-NANO** thermal label print bridge.

## Board

- **SoC:** ESP32-P4 (IDF target `esp32p4`)
- **Wireless:** ESP32-C6 over SDIO (Wi-Fi 6 / BLE) via `esp_hosted` + `esp_wifi_remote`
- **USB-A:** USB 2.0 OTG High Speed host → thermal printer
- **Ethernet:** onboard 100 Mbps RJ45
- Board docs: https://docs.waveshare.com/ESP32-P4-NANO
- Local cheat sheet: [`../docs/board.md`](../docs/board.md)

## Toolchain

ESP-IDF **>= 5.5** (verified on **v6.0.1**).

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

## Modules

| Module | File | Status |
|--------|------|--------|
| USB HS printer class | `main/usb_printer.c` | Bulk OUT, PSRAM ring |
| Ethernet, then Wi-Fi | `main/net_bringup.c` | Onboard RJ45 preferred, C6 Wi-Fi fallback |
| Wi-Fi setup page | `main/portal.c` | Port 80 on Ethernet; open setup network only with no cable |
| TCP 9100 listener | `main/net_9100.c` | Raw bytes to the printer, replies back to the client |
| IPP Everywhere | `main/ipp_server.c` | Port 631, `_ipp._tcp` |
| Raster to ZPL | `main/raster_zpl.c` | `image/pwg-raster` and `image/urf` to a 4x6 `^GFA` via `label_toolkit` |
| App update | `main/app_ota.c` | Closed until `firmware/.ota-token` exists |
| Dual OTA partitions | `partitions.csv` | `ota_0` / `ota_1` on 16 MB flash |

## Host smoke test

From the repository root (no IDF required):

```bash
python3 scripts/send_9100.py --host BOARD_IP
```
