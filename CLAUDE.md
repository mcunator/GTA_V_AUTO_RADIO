# CLAUDE.md — GTA_V_AUTO_RADIO

## Project Overview

GTA_V_AUTO_RADIO is a dual-microcontroller embedded project implementing a GTA V-themed Bluetooth auto-radio device. It runs on two separate ESP32 boards that communicate over UART:

- **`player_part`** — Audio engine: reads WAV files from an SD card, streams audio over Bluetooth A2DP (Classic BT) as a source device.
- **`ui_part`** — Display controller: renders a 240×240 LVGL v8 UI, handles rotary encoder and push-button input, and fetches album art and state from the player.

Both parts are Arduino sketches (`.ino`) with associated `.cpp`/`.h` support files. There is no build system (no Makefile, CMakeLists, or PlatformIO config); projects are compiled via the Arduino IDE or arduino-cli.

---

## Repository Structure

```
GTA_V_AUTO_RADIO/
├── player_part/                  # ESP32 audio player sketch
│   ├── player_part.ino           # Main sketch: SD scan, A2DP, audio loop
│   ├── player_rpc.cpp            # RPC server: UART parser + command handlers
│   ├── player_rpc.h              # Shared RPC protocol definitions (server side)
│   ├── config.h                  # A2DP library compile-time config flags
│   ├── AudioOutputA2DP.cpp/.h    # Custom AudioOutput adapter for A2DP
│   ├── BluetoothA2DP*.cpp/.h     # Vendored BluetoothA2DP library (pschatzmann)
│   └── A2DPVolumeControl.h       # Volume control interface
│
└── ui_part/                      # ESP32-S3 (or S2/C3) display sketch
    ├── ui_part.ino               # Main sketch: LVGL screens, input, RPC calls
    ├── player_rpc.cpp            # RPC client: UART parser + command senders
    ├── player_rpc.h              # Shared RPC protocol definitions (client side)
    ├── lvgl_v8_port.cpp/.h       # LVGL porting layer (tick task, mutex, display init)
    ├── lv_conf.h                 # LVGL v8 compile-time configuration
    ├── esp_panel_board_supported_conf.h   # esp_display_panel board config
    ├── esp_panel_board_custom_conf.h      # Custom board overrides
    ├── esp_panel_drivers_conf.h           # Display/touch driver selection
    ├── esp_utils_conf.h                   # ESP utility library config
    └── images/
        ├── connected.h/.png      # BT connected icon (LVGL image descriptor)
        ├── disconnected.h/.png   # BT disconnected icon
        ├── nosd.h/.png           # No SD card splash screen
        └── powerup.h/.png        # Boot/loading splash screen
```

---

## Architecture

### Inter-MCU Communication: Custom UART RPC Protocol

Both halves share a binary UART protocol defined identically in `player_rpc.h` (both copies must stay in sync). Key constants:

| Constant | Value |
|---|---|
| `RPC_UART_BAUDRATE` | 921600 baud |
| `RPC_MAGIC` | `0xA5` |
| `RPC_VERSION` | `0x01` |
| `RPC_MAX_PAYLOAD` | 1024 bytes |

**Frame format:** `[RpcHeader (8 bytes)] [payload (0–1024 bytes)] [CRC-16/CCITT (2 bytes)]`

```c
struct RpcHeader {
  uint8_t  magic;    // Always 0xA5
  uint8_t  version;  // Always 0x01
  uint16_t length;   // Payload byte count
  uint16_t seq;      // Sequence number
  uint8_t  type;     // RpcType enum
  uint8_t  opcode;   // RpcOpcode enum
};
```

All structs in the protocol use `#pragma pack(push, 1)` — **never add padding**.

**Packet types (`RpcType`):**

| Type | Direction | Description |
|---|---|---|
| `RPC_CMD` (0x01) | UI → Player | Command requesting an action |
| `RPC_RESP` (0x02) | Player → UI | Response to a command (same seq) |
| `RPC_EVENT` (0x03) | Player → UI | Unsolicited state change notification |
| `RPC_PING` (0x04) | UI → Player | Heartbeat (every 500 ms) |
| `RPC_PONG` (0x05) | Player → UI | Heartbeat reply |

**Connection health:** The UI side tracks `pingPckt` and `pongPckt` counters. `rpc_intercom_is_active()` returns `true` only after 10+ pings with at most 2 unanswered pings. This guards all state transitions in `loop()`.

**UART pins:**
- `player_part`: `Serial2` — RX=GPIO16, TX=GPIO17
- `ui_part`: `Serial0` — default pins (typically GPIO3/GPIO1 on ESP32)

### SD Card Layout (player_part)

The SD card root must contain flat directories. Each directory represents a **radio station / folder** and must contain:

- Exactly one `.wav` file — 44100 Hz, 16-bit stereo, raw PCM (no seek header skipping is implemented beyond raw byte offset).
- Optionally one `.cover` file — 240×240 raw 1bpp bitmap: each bit represents one pixel (0=black, 1=white), packed into `240*240/8 = 7200` bytes, stored row-major, LSB-first within each byte.

Maximum folders: `MAX_FOLDERS = 20`. Folders with no `.wav` file are silently skipped.

### Album Art Streaming

The UI requests album art via a chunked RPC flow:
1. UI sends `RPC_GET_FOLDER_IMAGE_HEADER` → player ACKs.
2. UI sends repeated `RPC_GET_FOLDER_IMAGE_CHUNK` with `{offset, size}` → player responds with raw bytes from the 1bpp buffer.
3. Chunk size is `IMAGE_CHUNK_SIZE = RPC_MAX_PAYLOAD * 3 / 4 = 768` bytes.
4. When `nextBytes == BITMAP_SIZE (7200)`, UI expands the 1bpp buffer to RGB565 (white=`0xFFFF`, black=`0x0000`) into `album_buffer[240*240]` and calls `ui_change_image()`.

### Audio Playback (player_part)

- Audio task runs on **core 1** via `xTaskCreatePinnedToCore`, priority 5.
- Commands arrive via `mp3CmdQueue` (MP3_PLAY / MP3_PAUSE / MP3_STOP).
- Events sent back to the main loop via `mp3EventQueue` (track finished, AVRCP commands).
- Playback offset is calculated from a global wall-clock `startPlayingTime` so each radio station plays as if it has been running continuously since the device booted (GTA radio simulation).
- **Track step time:** `TRACK_STEP_TIME = 120000 ms` — forward/back skips 2 minutes in the continuous-play timeline.

### Bluetooth (player_part)

The player acts as an **A2DP Source** (not sink) — it initiates the connection to a headset/speaker. It uses the `BluetoothA2DPSource` class from the vendored pschatzmann library.

Connection management alternates every 10 seconds between:
- **Discovery mode** — scans for bonded/saved devices and attempts `connect_to()`.
- **Scannable/connectable mode** — device is visible for pairing.

The UI can trigger BT scan (`RPC_BT_START_DISCOVERY`), get discovered devices (`RPC_BT_GET_SCAN_LIST`), save a device to connect (`RPC_BT_SAVE_TO_CONNECT`), and clear all bonding (`RPC_BT_CLEAR_PAIRED`).

### UI Screens (ui_part)

All screens are created once at startup in `ui_init()` and switched via `lv_scr_load_anim()` with a 300 ms fade:

| Screen | Enum | Shown when |
|---|---|---|
| Loading / splash | `UI_LOADING` | RPC not yet active or lost |
| Player | `UI_PLAYER` | Normal playback |
| Bluetooth list | `UI_BT` | User long-presses encoder |
| No SD card | `UI_NO_SD` | `sd_mounted == false` in state |

### Input Handling (ui_part)

Input events are funneled through a FreeRTOS queue (`gesturesQueue`) of `Gestures_e`:

| Gesture | Source |
|---|---|
| `L_gesture` | Encoder left turn (debounced via 100 ms timer) |
| `R_gesture` | Encoder right turn |
| `TAP_gesture` | Single button click |
| `TAPx2_gesture` | Double button click |
| `LONG_gesture` | Long press start |

The encoder uses GPIO 6 (pin A) and GPIO 7 (pin B). The button uses GPIO 9 (active-low).

**Gesture → action mapping per screen:**

| Gesture | UI_PLAYER | UI_BT |
|---|---|---|
| Left | Previous folder | Move selection up |
| Right | Next folder | Move selection down |
| Tap | Next track (+2 min) | Confirm (pair/remove) |
| Double-tap | Previous track (−2 min) | Rescan BT |
| Long | Open BT screen + start scan | Close BT screen |

---

## Key Conventions

### General

- **Language:** C++11/14, Arduino framework, ESP-IDF underneath.
- **Style:** K&R-ish braces, 2-space indentation (mixed — follow the surrounding code).
- **No dynamic memory in hot paths:** Prefer stack or static allocations. The BT scan list and folder list are fixed-size static arrays.
- **FreeRTOS primitives:** Use queues for inter-task communication; never share mutable state between tasks/ISRs without a queue or mutex.
- **LVGL thread safety:** Always wrap LVGL API calls outside the LVGL task with `lvgl_port_lock(-1)` / `lvgl_port_unlock()`.

### RPC Protocol Rules

- The `player_rpc.h` header is **duplicated** in both projects and must be kept byte-for-byte identical in the shared definitions (protocol constants, structs, enums).
- Each side has side-specific additions: `player_part/player_rpc.h` declares `button_handler`, `getCurrentAlbumFile`, `bt_*` functions; `ui_part/player_rpc.h` declares UI callback declarations and RPC client helpers.
- CRC-16/CCITT is computed over header + payload. CRC seed is `0xFFFF`. Polynomial `0x1021`.
- Responses always echo the `seq` from the originating command (`seq_override` parameter).

### Image Format

Cover art files (`.cover`) must be:
- Exactly `240 * 240 / 8 = 7200` bytes.
- 1bpp, row-major, LSB-first bit order within each byte.
- Pixel value: `1` = white (`0xFFFF` RGB565), `0` = black (`0x0000`).

### SD Card

- Audio files must use the `.wav` extension (despite the `isMP3()` function name — it checks `.wav`).
- Cover files use the `.cover` extension.
- Only the first `.cover` found per directory is used.
- Directory names are not displayed; only one track per folder is used (last `.wav` wins if multiple exist).

---

## Hardware Targets

| Part | Board |
|---|---|
| `player_part` | ESP32 (classic, dual-core) |
| `ui_part` | ESP32-S3 or compatible with 240×240 SPI/RGB LCD |

**`player_part` pinout:**

| Signal | GPIO |
|---|---|
| SD MOSI | 23 |
| SD MISO | 19 |
| SD SCK | 18 |
| SD CS | 5 |
| RPC UART RX | 16 |
| RPC UART TX | 17 |

**`ui_part` pinout:**

| Signal | GPIO |
|---|---|
| Encoder A | 6 |
| Encoder B | 7 |
| Button | 9 |
| RPC UART | Serial0 (default pins) |
| LCD/Touch | Configured via `esp_panel_board_*_conf.h` |

---

## Dependencies

### player_part

- [ESP32-A2DP (pschatzmann)](https://github.com/pschatzmann/ESP32-A2DP) — vendored in `player_part/Bluetooth*.cpp/.h`
- [ESP8266Audio / ESP32Audio](https://github.com/earlephilhower/ESP8266Audio) — `AudioGeneratorWAV`, `AudioFileSourceSD`
- Arduino SD library
- ESP-IDF Bluetooth stack (`esp_bt_main`, `esp_gap_bt_api`, `esp_avrc_api`)

### ui_part

- [LVGL v8](https://github.com/lvgl/lvgl) — configured via `lv_conf.h`
- [esp_display_panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel) — board abstraction for LCD + touch
- [ESP_Knob](https://github.com/esp-arduino-libs/esp-iot-solution) — rotary encoder driver
- [Button](https://github.com/esp-arduino-libs/esp-iot-solution) — debounced button driver

---

## Development Notes

- There are no automated tests. Verification is done via Serial monitor at 921600 baud on both boards.
- Serial debug output uses `Serial.printf()`; both boards share the same baud rate (`921600`).
- The `connectFlip()` function in `player_part` has an incomplete branch (`if(isScannable) { }`) — this is intentional/WIP.
- Some commented-out LVGL label code in `ui_part.ino` (status label) is left as scaffolding for future track-name display.
- The `rpc_album_end()` call at the end of `rpc_album_chunk()` in `ui_part/player_rpc.cpp` is called unconditionally after every chunk — this may be a bug (it re-renders partial data). Do not "fix" this without understanding the full streaming flow.
- BLE memory is freed on the player side (`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)`) to give Classic BT more heap.
