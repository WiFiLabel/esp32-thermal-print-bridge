# ESP32 Thermal Print Bridge

> Turn a USB-only thermal label printer into a LAN JetDirect device (TCP **9100**) with ESP32 firmware. Ethernet + Wi-Fi path, USB host, ZPL / EPL / TSPL passthrough.

**Status:** early bring-up. TCP 9100 listener and USB host scaffold are in tree; end-to-end label print on hardware is the next milestone. Issues and PRs welcome.

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5%2B-blue.svg)](https://docs.espressif.com/projects/esp-idf/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/WiFiLabel/esp32-thermal-print-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/WiFiLabel/esp32-thermal-print-bridge/actions)

## What you get

- Raw **TCP 9100** (AppSocket / HP JetDirect) — works with `nc`, CUPS `socket://`, and shipping tools that already speak ZPL
- **USB High-Speed host** to the printer (USB Printer Class path)
- Target board: **Waveshare ESP32-P4-NANO** (ESP32-P4 + ESP32-C6 Wi-Fi 6, onboard RJ45 + USB-A)
- Label language helpers for **ZPL** (`^GFA`), **EPL2** (`GW`), **TSPL2** (`BITMAP`)
- Dual **OTA** partitions on 16 MB flash
- Host smoke test: `scripts/send_9100.py`

## Comparison

| | Raspberry Pi print server | ESP32-S3 WiFi print server | This project |
| --- | --- | --- | --- |
| Always-on Linux | Yes | No | No |
| TCP 9100 | Via CUPS / custom | Yes | Yes |
| Ethernet | Usually yes | Rare | **Onboard RJ45** |
| Wi-Fi | Yes | Yes | **Wi-Fi 6** (C6) |
| Label focus (ZPL/EPL/TSPL) | Depends | Often general / CDC-only | **Yes** |
| Typical power / size | Pi + SD | Dev board | Dev board → optional finished box |

## Hardware

| Part | Role |
| --- | --- |
| Waveshare [ESP32-P4-NANO](https://docs.waveshare.com/ESP32-P4-NANO) | MCU + Ethernet + Wi-Fi 6 |
| USB-C | Power, flash, serial monitor |
| USB-A | USB 2.0 HS **host** → thermal printer |
| RJ45 | Prefer for first bring-up |
| USB thermal label printer | Zebra / Rollo / Munbyn-class (raw language over USB) |

See [docs/board.md](docs/board.md) for ports and flash notes.

## Quick start

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **>= 5.5** (verified on **v6.0.1**).

```bash
. $HOME/esp/esp-idf/export.sh   # or your IDF install
cd firmware   # or repo root if firmware is at top level
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

On macOS, `PORT` is usually `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`.

Send a test job (host Python, no IDF):

```bash
python3 scripts/send_9100.py --host BOARD_IP
python3 scripts/send_9100.py --host BOARD_IP --file scripts/fixtures/hello.tspl
```

## How it works

```
[PC / phone / CUPS] --TCP :9100--> [ESP32-P4] --USB host--> [thermal printer]
```

1. Network stack brings up Ethernet (then Wi-Fi via onboard C6)
2. `net_9100` accepts one job per TCP connection and forwards bytes
3. `usb_printer` writes to the attached USB printer

## Development

```bash
idf.py build
idf.py -p PORT flash monitor
```

PRs welcome for printer VID/PID notes, CDC-ACM paths, and additional boards. Primary support target is the ESP32-P4-NANO; other ESP32 USB-OTG boards are community welcome.

## Prefer a finished unit?

DIY is fully supported here. If you want a finished enclosure, flash, and support, see **[wifilabel.com](https://wifilabel.com)** (WiFiLabel Box / Pro).

## License

MIT — see [LICENSE](LICENSE).

## Security

Please report vulnerabilities via [SECURITY.md](SECURITY.md).
