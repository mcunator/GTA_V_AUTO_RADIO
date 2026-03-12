#include <Arduino.h>
#include "esp_timer.h"
#include "BluetoothA2DPSource.h"
#include "player_rpc.h"

#define RPC_UART Serial2
static uint16_t g_seq = 1;
static RpcRxContext rx;
static PlayerState_t g_state = {
  .bt = BT_OFF,
  .playback = PB_STOPPED,
  .sd_mounted = false
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

/* ============================================================
   SEND
   ============================================================ */
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
  if (type != RPC_PONG) {
    Serial.printf("%d SENT PCKT %02X|%02X|%02x\n", millis(), type, opcode, len);
  }
}

/* ============================================================
   PAYLOAD TIMEOUT CALC
   ============================================================ */

static inline uint64_t now_us() {
  return esp_timer_get_time();
}

uint32_t calc_payload_timeout_us(uint16_t payload_len) {
  return payload_len * 10;
}

/* ============================================================
   HANDLERS
   ============================================================ */

void send_state_event() {
  rpc_send(RPC_EVENT, RPC_EVT_STATE_CHANGED,
           &g_state, sizeof(g_state));
}
void commonOkResponse(const RpcHeader* hdr, uint8_t isOk = 1) {
  Packet_t r = { .type = _ack };
  r.isok.ok = isOk;
  rpc_send(RPC_RESP, hdr->opcode, &r, sizeof(ExecResult_t) + sizeof(PacketType_e), hdr->seq);
}

/* ---- COMMAND HANDLERS ---- */
void handle_set_playback(const RpcHeader* hdr, const uint8_t* payload) {
  uint8_t isOk = 1;
  if (hdr->length == 1) {
    int8_t upDown = *(int8_t*)payload;
    button_handler(upDown > 0 ? ESP_AVRC_PT_CMD_PLAY : ESP_AVRC_PT_CMD_PAUSE, false);
  } else {
    isOk = 0;
  }
  commonOkResponse(hdr, isOk);
}

void handle_set_folder(const RpcHeader* hdr, const uint8_t* payload) {
  uint8_t isOk = 1;
  if (hdr->length == 1) {
    int8_t upDown = *(int8_t*)payload;
    button_handler(upDown > 0 ? ESP_AVRC_PT_CMD_FORWARD : ESP_AVRC_PT_CMD_BACKWARD, false);
  } else {
    isOk = 0;
  }
  commonOkResponse(hdr, isOk);
}

void handle_set_track(const RpcHeader* hdr, const uint8_t* payload) {
  uint8_t isOk = 1;
  if (hdr->length == 1) {
    int8_t upDown = *(int8_t*)payload;
    button_handler(upDown > 0 ? ESP_AVRC_PT_CMD_FAST_FORWARD : ESP_AVRC_PT_CMD_REWIND, false);
  } else {
    isOk = 0;
  }
  commonOkResponse(hdr, isOk);
}

void handle_bt_start_discovery(BluetoothA2DPSource* src, const RpcHeader* hdr) {
  bt_set_scan_state(1);
  src->set_auto_reconnect(false);
  if (src->is_connected()) {
    src->disconnect();
  }
  // cancel_discovery sets is_end=true so library won't auto-restart;
  // then start a fresh inquiry so ssid_callback fires for nearby devices.
  src->cancel_discovery();
  esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  commonOkResponse(hdr);
}

void handle_bt_connect(BluetoothA2DPSource* src, const RpcHeader* hdr, const uint8_t* payload) {
  Packet_t* pckt = (Packet_t*)payload;
  uint8_t isOk = 1;
  if (pckt->type == _btConnect) {
    if (src->is_connected()) {
      src->disconnect();
    }
    src->set_auto_reconnect(true);
    bt_set_to_connect(pckt->bt.mac);
    src->connect_to(pckt->bt.mac);
  } else {
    isOk = 0;
  }
  commonOkResponse(hdr, isOk);
}

void handle_bt_clear_paired(BluetoothA2DPSource* src, const RpcHeader* hdr) {
  if (src->is_connected()) {
    src->set_auto_reconnect(false);
    src->disconnect();
  }

  int dev_num = esp_bt_gap_get_bond_device_num();
  if (dev_num != ESP_FAIL && dev_num > 0) {
    esp_bd_addr_t* dev_list = (esp_bd_addr_t*)malloc(dev_num * sizeof(esp_bd_addr_t));
    if (dev_list) {
      memset((uint8_t*)dev_list, 0, dev_num * sizeof(esp_bd_addr_t));
      if (esp_bt_gap_get_bond_device_list(&dev_num, dev_list) == ESP_OK) {
        for (int i = 0; i < dev_num; i++) {
          esp_bt_gap_remove_bond_device(dev_list[i]);
        }
      }
      free(dev_list);
    }
  }
  commonOkResponse(hdr);
}

void handle_bt_get_scan(const RpcHeader* hdr) {
  Packet_t r = { .type = _btScan };
  uint8_t listSize = 0;
  bt_get_scan_list(&listSize, &r.scan);
  rpc_send(RPC_RESP, hdr->opcode, &r, sizeof(ScanItem_t) * listSize + sizeof(PacketType_e), hdr->seq);
}
void handle_bt_end_scan(BluetoothA2DPSource* src, const RpcHeader* hdr) {
  bt_set_scan_state(0);
  int dev_num = esp_bt_gap_get_bond_device_num();
  if (dev_num != ESP_FAIL && dev_num > 0) {
    src->set_auto_reconnect(true);
    esp_bd_addr_t* dev_list = (esp_bd_addr_t*)malloc(dev_num * sizeof(esp_bd_addr_t));
    if (dev_list) {
      memset((uint8_t*)dev_list, 0, dev_num * sizeof(esp_bd_addr_t));
      if (esp_bt_gap_get_bond_device_list(&dev_num, dev_list) == ESP_OK) {
        src->connect_to(dev_list[0]);
      }
      free(dev_list);
    }
  }
  commonOkResponse(hdr);
}

static void rpc_album_begin(const RpcHeader* hdr) {
  commonOkResponse(hdr);
}
static void rpc_album_chunk(const RpcHeader* hdr, const uint8_t* p) {
  Packet_t r = { .type = _imageChunk };
  Packet_t* pckt = (Packet_t*)p;
  ImageChunk_t* c = &pckt->chunk;
  uint16_t* buf;
  uint8_t* image = getCurrentAlbumFile();
  int chunkSize;
  if (hdr->length != sizeof(ImageChunk_t) + sizeof(PacketType_e) || pckt->type != _imageChunk) {
    goto error;
  }
  if (c->offset > MAX_IMAGE_SIZE || c->size > MAX_IMAGE_SIZE) {
    goto error;
  }
  chunkSize = c->offset + c->size > MAX_IMAGE_SIZE ? MAX_IMAGE_SIZE - c->offset : c->size;
  r.chunk.offset = c->offset;
  r.chunk.size = chunkSize;
  memcpy(r.raw + sizeof(ImageChunk_t), &image[c->offset], chunkSize);
  rpc_send(RPC_RESP, hdr->opcode, &r, sizeof(ImageChunk_t) + sizeof(PacketType_e) + chunkSize, hdr->seq);
  return;
error:
  commonOkResponse(hdr, 0);
}
static void rpc_album_end(const RpcHeader* hdr) {
  commonOkResponse(hdr);
}
/* ============================================================
   DISPATCH
   ============================================================ */

void rpc_dispatch(BluetoothA2DPSource* src, const RpcHeader* hdr, const uint8_t* payload) {
  if (hdr->type == RPC_PING) {
    rpc_send(RPC_PONG, 0, &g_state, sizeof(g_state), hdr->seq);
    return;
  }

  Serial.printf("%d RECV PCKT %02X|%02X|%02X|%02X\n", millis(), hdr->type, hdr->opcode, hdr->length, payload ? payload[0] : 0);

  if (hdr->type != RPC_CMD)
    return;

  switch (hdr->opcode) {
    case RPC_SET_FOLDER: handle_set_folder(hdr, payload); break;
    case RPC_SET_TRACK: handle_set_track(hdr, payload); break;
    case RPC_SET_PLAYBACK: handle_set_playback(hdr, payload); break;
    case RPC_GET_FOLDER_IMAGE_HEADER: rpc_album_begin(hdr); break;
    case RPC_GET_FOLDER_IMAGE_CHUNK: rpc_album_chunk(hdr, payload); break;
    case RPC_GET_FOLDER_IMAGE_END: rpc_album_end(hdr); break;


    case RPC_BT_START_DISCOVERY: handle_bt_start_discovery(src, hdr); break;
    case RPC_BT_SAVE_TO_CONNECT: handle_bt_connect(src, hdr, payload); break;
    case RPC_BT_CLEAR_PAIRED: handle_bt_clear_paired(src, hdr); break;
    case RPC_BT_GET_SCAN_LIST: handle_bt_get_scan(hdr); break;
    case RPC_BT_END_SCAN: handle_bt_end_scan(src, hdr); break;
    default: break;
  }
}

/* ============================================================
   BYTE-BY-BYTE PARSER
   ============================================================ */

void rpc_rx_byte(BluetoothA2DPSource* src, uint8_t b) {
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
          rx.state = RX_WAIT_MAGIC;
          Serial.println("RX ERR 1");
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
        rx.state = RX_WAIT_MAGIC;
        Serial.printf("RX ERR 2 %d/%d\n", rx.hdr.length, rx.payload_pos);
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
          rpc_dispatch(src, &rx.hdr, rx.payload);
        } else {
          Serial.println("RX ERR 3");
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
  BluetoothA2DPSource* src = (BluetoothA2DPSource*)arg;
  src->get_connection_state();
  src->get_audio_state();
  src->get_last_peer_address();
  src->is_discovery_active();
  while (RPC_UART.available()) {
    rpc_rx_byte(src, RPC_UART.read());
  }
}

/* ============================================================
   INIT
   ============================================================ */

void rpc_init() {
#define RXD2 16
#define TXD2 17
  RPC_UART.setRxBufferSize(RPC_MAX_PAYLOAD);
  RPC_UART.setTxBufferSize(RPC_MAX_PAYLOAD);
  RPC_UART.begin(RPC_UART_BAUDRATE, SERIAL_8N1, RXD2, TXD2);
}
