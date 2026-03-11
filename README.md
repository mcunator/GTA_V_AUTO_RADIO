Here's a concise overview of the repository:

---

## GTA_V_AUTO_RADIO

**What it does:** A dual-ESP32 embedded system that emulates a GTA V-style in-car radio. It reads WAV audio tracks and album art from an SD card, streams audio over Bluetooth A2DP to a connected speaker/headphones, and displays a UI on a small round display. Users control it with a rotary knob and button.

---

## Architecture — Two ESP32 MCUs

The project is split across two firmware sketches that communicate over UART via a custom RPC protocol:

### `player_part/` — Audio/Bluetooth ESP32
- **Entry point:** `player_part.ino` (`setup()` / `loop()`)
- Mounts SD card over SPI, scans for folders containing `.wav` tracks and `.cover` album art images
- Streams WAV audio via Bluetooth A2DP **source** (acts as a Bluetooth audio sender)
- Handles AVRCP remote control commands (play/pause/next/prev)
- Manages BT device discovery, pairing, and auto-reconnect logic
- Runs a dedicated FreeRTOS task (`mp3Task`) on core 1 for audio decoding

### `ui_part/` — Display/UI ESP32
- **Entry point:** `ui_part.ino` (`setup()` / `loop()`)
- Drives a 240×240 display using **LVGL v8** via the `esp_display_panel` framework
- Manages four screens: Loading, Player (album art + BT status), Bluetooth device list, No-SD-card
- Reads a rotary encoder (knob left/right) and button (single/double/long click) via FreeRTOS queues
- Sends user gestures as RPC commands to the player ESP32

### `player_rpc.cpp/.h` — Inter-MCU Communication
- Custom binary RPC protocol over UART at 921600 baud
- Magic byte `0xA5`, versioned headers, CRC, sequenced packets
- Opcodes for: set folder/track, get album image chunks, BT scan/connect/clear

---

## Tech Stack

| Layer | Technology |
|---|---|
| MCU | ESP32 (dual) |
| RTOS | FreeRTOS (queues, tasks, timers) |
| Audio | `AudioGeneratorWAV` + `AudioOutputA2DP` (ESP8266Audio-style libs) |
| Bluetooth | Classic BT A2DP Source + AVRCP (ESP-IDF `esp_bt`, `esp_gap_bt_api`) |
| UI | LVGL v8, `esp_display_panel`, `ESP_Knob`, `Button` |
| Storage | SD card over SPI |
| IPC | Custom binary UART RPC |
| Language | Arduino/C++ (`.ino` + `.cpp`/`.h`) |

---

## Data Flow Summary

```
SD Card (.wav + .cover)
       ↓
  player_part ESP32  →  Bluetooth A2DP  →  Speaker/Headphones
       ↑↓ UART RPC
   ui_part ESP32  ←  Knob/Button input
       ↓
  240×240 Display (album art, BT list)
```
