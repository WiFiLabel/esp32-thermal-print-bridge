# firmware / esp32-p4-nano

ESP-IDF firmware for **WiFiLabel Box / Pro** on the **Waveshare ESP32-P4-NANO** development board.

## Board notes

- **SoC:** ESP32-P4 (IDF target `esp32p4`), RISC-V dual-core HP + LP core
- **Wireless:** ESP32-C6 over SDIO (Wi-Fi 6, Bluetooth 5 / BLE) via `esp_hosted` + `esp_wifi_remote`
- **USB:** Type-A USB 2.0 OTG High Speed (printer class host to Zebra/Rollo)
- **Ethernet:** onboard 100 Mbps RJ45
- **Flash / PSRAM:** 16 MB NOR, 32 MB PSRAM
- Board docs: https://docs.waveshare.com/ESP32-P4-NANO
- Local pin/port cheat sheet: [`../../doc/board-esp32-p4-nano.md`](../../doc/board-esp32-p4-nano.md)

MIPI CSI/DSI, audio, TF, and PoE headers are unused in print-bridge v1.

## Toolchain

ESP-IDF **>= 5.5** (verified on **v6.0.1**).

```bash
# once per shell
. ~/esp/esp-idf-v6/export.sh

cd firmware/esp32-p4-nano
idf.py set-target esp32p4   # first time only
idf.py build
idf.py -p PORT flash monitor
```

On macOS, `PORT` is usually `/dev/cu.usbmodem*` or `/dev/cu.usbserial*` for the board USB-C UART.

## Current modules

| Module | File | Status |
|--------|------|--------|
| USB HS host scaffold | `usb_printer.c` | Host lib installed; printer class BULK OUT stub |
| TCP 9100 listener | `net_9100.c` | Accept + forward bytes to `usb_printer_write` |
| Label wrappers | `label_proto.c` | ZPL `^GFA`, EPL2 `GW`, TSPL2 `BITMAP` |
| Dual OTA partitions | `partitions.csv` | `ota_0` / `ota_1` on 16 MB flash |
| mDNS | - | Planned |
| Ethernet / Wi-Fi bring-up | - | Planned (day 1+) |
| BLE provisioning | - | Later |
| Cloud client | - | Later |

## Host helpers

From repo root (no IDF required):

```bash
python3 scripts/send_9100.py --host BOARD_IP
python3 scripts/provision_unit.py --dry-run
```
