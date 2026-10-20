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
| USB HS host scaffold | `main/usb_printer.c` | Host lib installed; printer class BULK OUT stub |
| TCP 9100 listener | `main/net_9100.c` | Accept + forward bytes to `usb_printer_write` |
| Label wrappers | `main/label_proto.c` | ZPL `^GFA`, EPL2 `GW`, TSPL2 `BITMAP` |
| Dual OTA partitions | `partitions.csv` | `ota_0` / `ota_1` on 16 MB flash |
| Ethernet / Wi-Fi / mDNS | - | Planned |

## Host smoke test

From the repository root (no IDF required):

```bash
python3 scripts/send_9100.py --host BOARD_IP
```
