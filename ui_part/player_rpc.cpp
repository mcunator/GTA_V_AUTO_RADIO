#include <Arduino.h>
#include "esp_timer.h"
#include "player_rpc.h"
#include "lvgl.h"

#define RPC_UART Serial0
static uint16_t g_seq = 1;
static RpcRxContext rx;

static PlayerState_t g_state = {
  .bt = BT_OFF,
  .playback = PB_STOPPED,
  .sd_mounted = false,
  .currentFolder = 10000,
};

uint16_t rpc_crc16(const uint8_t* data, size_t len, uint16_t seed = 0xFFFF) {
  uint16_t crc = seed;
  while (len--) {
    crc ^= (*data++) << 8;
    for (int i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

static void send_arr(uint8_t* arr, uint16_t size) {
  int sended = 0;
  while (size) {
    int avalable = RPC_UART.availableForWrite();
    int toSent = avalable > size ? size : avalable;
    if (toSent == 0) {
      delay(1);
      continue;
    }
    RPC_UART.write(arr + sended, toSent);
    sended += toSent;
    size -= toSent;
  }
}

void rpc_send(uint8_t type,
              uint8_t opcode,
              const void* payload,
              uint16_t len,
              uint16_t seq_override = 0) {
  RpcHeader hdr;
  hdr.magic = RPC_MAGIC;
  hdr.version = RPC_VERSION;
  hdr.length = len;
  hdr.seq = seq_override ? seq_override : g_seq++;
  hdr.type = type;
  hdr.opcode = opcode;
  uint16_t crc;
  send_arr((uint8_t*)&hdr, sizeof(hdr));
  if (len && payload)
    send_arr((uint8_t*)payload, len);

  crc = rpc_crc16((uint8_t*)&hdr, sizeof(hdr));
  if (len) crc = rpc_crc16((uint8_t*)payload, len, crc);
  send_arr((uint8_t*)&crc, sizeof(crc));
  if (type != RPC_PING) {
    Serial.printf("%d SENT PCKT %02X|%02X|%02x\n", millis(), type, opcode, len);
  }
}
uint32_t calc_payload_timeout_us(uint16_t payload_len) {
  return payload_len * 10;
}
void send_state_event() {
  rpc_send(RPC_EVENT, RPC_EVT_STATE_CHANGED,
           &g_state, sizeof(g_state));
}

#define SCHEDULE_GET_ALBUM 20
static uint32_t changedFolderTime = 0;

/* =========================
   ALBUM ART FSM
   ========================= */
#define IMAGE_CHUNK_SIZE (RPC_MAX_PAYLOAD * 3 / 4)
#define BITMAP_SIZE      (UI_w * UI_h / 8)
#define ALBUM_XFER_TIMEOUT_MS 3000

typedef enum {
  ALBUM_IDLE = 0,
  ALBUM_WAIT_HEADER_ACK,  // HEADER CMD sent, waiting for player ACK
  ALBUM_TRANSFERRING,     // Receiving chunks
} AlbumFsmState_e;

static AlbumFsmState_e albumState  = ALBUM_IDLE;
static uint32_t        albumTimeout = 0;
static uint32_t        nextBytes    = 0;
static uint8_t  album_compressed[BITMAP_SIZE] = {0};
static uint16_t album_buffer[UI_w * UI_h]     = {0};

void rpc_request_album_chunk(uint32_t offset, uint32_t len) {
  Packet_t p = {.type = _imageChunk};
  p.chunk.offset = offset;
  p.chunk.size   = len;
  rpc_send(RPC_CMD, RPC_GET_FOLDER_IMAGE_CHUNK, &p,
           sizeof(ImageChunk_t) + sizeof(PacketType_e), 0);
}

void rpc_set_album_begin(void) {
  albumState  = ALBUM_WAIT_HEADER_ACK;
  nextBytes   = 0;
  memset(album_compressed, 0, sizeof(album_compressed));
  albumTimeout = millis() + ALBUM_XFER_TIMEOUT_MS;
  rpc_send(RPC_CMD, RPC_GET_FOLDER_IMAGE_HEADER, 0, 0, 0);
}

void rpc_set_album_end(void) {
  rpc_send(RPC_CMD, RPC_GET_FOLDER_IMAGE_END, 0, 0, 0);
}

/* Called when HEADER ACK arrives from player */
static void rpc_album_on_header_ack(void) {
  if (albumState != ALBUM_WAIT_HEADER_ACK) return;
  albumState   = ALBUM_TRANSFERRING;
  albumTimeout = millis() + ALBUM_XFER_TIMEOUT_MS;
  rpc_request_album_chunk(0, IMAGE_CHUNK_SIZE);
}

static void rpc_render_album(void) {
  int32_t p = 0;
  for (int i = 0; i < BITMAP_SIZE; i++) {
    for (int j = 0; j < 8; j++, p++) {
      album_buffer[p] = (album_compressed[i] & (1 << j)) ? 0xFFFF : 0;
    }
  }
  ui_change_image(album_buffer);
}

/* Called when CHUNK RESP arrives from player */
static void rpc_album_on_chunk(const uint8_t* p, const RpcHeader* hdr) {
  if (albumState != ALBUM_TRANSFERRING) return;

  Packet_t* pckt = (Packet_t*)p;
  if (pckt->type != _imageChunk) return;

  /* Discard out-of-order responses (from an aborted transfer pipeline) */
  if (pckt->chunk.offset != nextBytes) {
    rpc_request_album_chunk(nextBytes, IMAGE_CHUNK_SIZE);
    return;
  }
  if (pckt->chunk.offset + pckt->chunk.size > BITMAP_SIZE) return;

  uint8_t* data = (uint8_t*)&p[sizeof(ImageChunk_t) + sizeof(PacketType_e)];
  memcpy(album_compressed + pckt->chunk.offset, data, pckt->chunk.size);
  nextBytes    = pckt->chunk.offset + pckt->chunk.size;
  albumTimeout = millis() + ALBUM_XFER_TIMEOUT_MS;

  if (nextBytes >= BITMAP_SIZE) {
    rpc_render_album();
    rpc_set_album_end();
    albumState = ALBUM_IDLE;
  } else {
    rpc_request_album_chunk(nextBytes, IMAGE_CHUNK_SIZE);
  }
}

static void album_fsm_tick(void) {
  if (albumState != ALBUM_IDLE && (int32_t)(millis() - albumTimeout) > 0) {
    Serial.printf("Album FSM timeout, state=%d\n", albumState);
    albumState = ALBUM_IDLE;
  }
}

void handle_set_folder(const RpcHeader* hdr, const uint8_t* payload) {
  /* Abort any in-progress transfer so stale responses are discarded */
  albumState       = ALBUM_IDLE;
  changedFolderTime = millis() + SCHEDULE_GET_ALBUM;
}

void handle_set_track(const RpcHeader* hdr, const uint8_t* payload) {
}

void handle_bt_start_discovery(const RpcHeader* hdr) {
}

void handle_bt_connect(const RpcHeader* hdr) {
}

void handle_bt_clear_paired(const RpcHeader* hdr) {
}
#define MAC_STR "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_ARR(p) p[0], p[1], p[2], p[3], p[4], p[5]
void handle_bt_get_scan(const RpcHeader* hdr, const uint8_t * payload) {
  Packet_t * pckt = (Packet_t *)payload;
  if(pckt->type == _btScan) {
    int count = (hdr->length - sizeof(PacketType_e)) / sizeof(ScanItem_t);
    Serial.printf("Count %d\n", count);
    ScanItem_t *scan = &pckt->scan;
    ui_bt_list_begin_update();
    for(int i = 0; i < count; i++) {
      ui_add_bt_item(scan[i]);
      Serial.printf("%d %s " MAC_STR " %d\n", i, scan[i].name, MAC_ARR(scan[i].mac), scan[i].rssi);
    }
    ui_bt_list_end_update(count);
  }
}
void handle_bt_end_scan(const RpcHeader* hdr, const uint8_t * payload) {

}

void rpc_set_new_folder(int8_t dir) {
  rpc_send(RPC_CMD, RPC_SET_FOLDER, &dir, sizeof(dir), 0);
}
void rpc_set_new_track(int8_t dir) {
  rpc_send(RPC_CMD, RPC_SET_TRACK, &dir, sizeof(dir), 0);
}
void rpc_start_bt_scan(void) {
  rpc_send(RPC_CMD, RPC_BT_START_DISCOVERY, 0, 0, 0);
}
void rpc_get_bt_scan_list(void) {
  rpc_send(RPC_CMD, RPC_BT_GET_SCAN_LIST, 0, 0, 0);
}
void rpc_set_bt_end_scan(void) {
  rpc_send(RPC_CMD, RPC_BT_END_SCAN, 0, 0, 0);
}

static bool bt_scan_polling = false;
static uint32_t lastBtScanPollTime = 0;

void rpc_bt_poll_start(void) {
  bt_scan_polling = true;
  lastBtScanPollTime = 0;  // triggers immediately on first rpc_task tick
}

void rpc_bt_poll_stop(void) {
  bt_scan_polling = false;
}

void rpc_save_bt_item(uint8_t * mac) {
  Packet_t r = {.type = _btConnect};
  memcpy(r.bt.mac, mac, 6);
  rpc_send(RPC_CMD, RPC_BT_SAVE_TO_CONNECT, &r, sizeof(Connect_t) + sizeof(PacketType_e), 0);
}

void rpc_delete_bonding(void) {
  rpc_send(RPC_CMD, RPC_BT_CLEAR_PAIRED, 0, 0, 0);
}

/* ============================================================
   DISPATCH
   ============================================================ */
static uint32_t lastPingTime = 0;
static int pingPckt = 0;
static int pongPckt = 0;
bool rpc_intercom_is_active(void) {
  return pingPckt > 10 && abs(pingPckt - pongPckt) < 3;
}

static void rpc_handle_state_event(const RpcHeader* hdr, const uint8_t* p) {
}

void rpc_dispatch(const RpcHeader* hdr, const uint8_t* payload) {
  lastPingTime = millis();
  if (hdr->type == RPC_PONG) {
    pongPckt = pingPckt;
    return;
  }
  Serial.printf("%d RECV PCKT %02X|%02X|%02X|%02X\n", millis(), hdr->type, hdr->opcode, hdr->length, payload ? payload[0] : 0);
  if (hdr->type == RPC_CMD)
    return;
  switch (hdr->opcode) {
    case RPC_SET_FOLDER:              handle_set_folder(hdr, payload); break;
    case RPC_SET_TRACK:               handle_set_track(hdr, payload); break;
    case RPC_GET_FOLDER_IMAGE_HEADER: rpc_album_on_header_ack(); break;
    case RPC_GET_FOLDER_IMAGE_CHUNK:  rpc_album_on_chunk(payload, hdr); break;
    case RPC_GET_FOLDER_IMAGE_END:    /* player acked our END signal */ break;

    case RPC_EVT_STATE_CHANGED: rpc_handle_state_event(hdr, payload); break;

    case RPC_BT_START_DISCOVERY: handle_bt_start_discovery(hdr); break;
    case RPC_BT_SAVE_TO_CONNECT: handle_bt_connect(hdr); break;
    case RPC_BT_CLEAR_PAIRED:    handle_bt_clear_paired(hdr); break;
    case RPC_BT_GET_SCAN_LIST:   handle_bt_get_scan(hdr, payload); break;
    case RPC_BT_END_SCAN:        handle_bt_end_scan(hdr, payload); break;
    default: break;
  }
}

/* ============================================================
   BYTE-BY-BYTE PARSER
   ============================================================ */

void rpc_rx_byte(uint8_t b) {

  bool rxError = false;
  switch (rx.state) {

    case RX_WAIT_MAGIC:
      if (b == RPC_MAGIC) {
        rx.hdr_pos = 0;
        ((uint8_t*)&rx.hdr)[rx.hdr_pos++] = b;
        rx.state = RX_HEADER;
      }
      break;

    case RX_HEADER:
      ((uint8_t*)&rx.hdr)[rx.hdr_pos++] = b;

      if (rx.hdr_pos == sizeof(RpcHeader)) {

        if (rx.hdr.magic != RPC_MAGIC || rx.hdr.version != RPC_VERSION || rx.hdr.length > RPC_MAX_PAYLOAD) {
          Serial.println("RX ERR 1");
          rx.state = RX_WAIT_MAGIC;
          rxError = true;
          break;
        }

        if (rx.hdr.length == 0) {
          rx.crc_pos = 0;
          rx.state = RX_CRC;
        } else {
          rx.payload_pos = 0;
          rx.payload_deadline =
            millis() + calc_payload_timeout_us(rx.hdr.length);
          rx.state = RX_PAYLOAD;
        }
      }
      break;

    case RX_PAYLOAD:
      if (millis() > rx.payload_deadline) {
        Serial.printf("RX ERR 2 %d/%d\n", rx.hdr.length, rx.payload_pos);
        rx.state = RX_WAIT_MAGIC;
        rxError = true;
        break;
      }

      rx.payload[rx.payload_pos++] = b;

      if (rx.payload_pos == rx.hdr.length) {
        rx.crc_pos = 0;
        rx.state = RX_CRC;
      }
      break;

    case RX_CRC:
      rx.crc_buf[rx.crc_pos++] = b;

      if (rx.crc_pos == 2) {

        uint16_t crc_rx =
          rx.crc_buf[0] | (rx.crc_buf[1] << 8);

        uint16_t crc_calc =
          rpc_crc16((uint8_t*)&rx.hdr, sizeof(RpcHeader));

        if (rx.hdr.length)
          crc_calc = rpc_crc16(rx.payload, rx.hdr.length, crc_calc);

        if (crc_rx == crc_calc) {
          rpc_dispatch(&rx.hdr, rx.payload);
        } else {
          Serial.println("RX ERR 3");
          rxError = true;
        }

        rx.state = RX_WAIT_MAGIC;
      }
      break;
  }
}

/* ============================================================
   TASK
   ============================================================ */

void rpc_task(void* arg) {
  while (RPC_UART.available()) {
    rpc_rx_byte(RPC_UART.read());
  }
  uint32_t nowMs = millis();
  if (nowMs - lastPingTime > 500) {
    lastPingTime = nowMs;
    pingPckt++;
    rpc_send(RPC_PING, 0, 0, 0, pingPckt);
  }

  if (changedFolderTime > 0 && changedFolderTime < millis()) {
    rpc_set_album_begin();
    changedFolderTime = 0;
  }

  if (bt_scan_polling && nowMs - lastBtScanPollTime >= 2000) {
    lastBtScanPollTime = nowMs;
    rpc_get_bt_scan_list();
  }
  album_fsm_tick();
}

/* ============================================================
   INIT
   ============================================================ */

void rpc_init() {
  RPC_UART.setRxBufferSize(RPC_MAX_PAYLOAD);
  RPC_UART.setTxBufferSize(RPC_MAX_PAYLOAD);
  RPC_UART.begin(RPC_UART_BAUDRATE, SERIAL_8N1);
}
