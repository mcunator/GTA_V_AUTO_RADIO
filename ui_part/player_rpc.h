#ifndef __PLAYER_RPC_HEADER
#define __PLAYER_RPC_HEADER

#define UI_w 240
#define UI_h 240

#define MAX_SCAN_ITEMS 10
#define BT_MAX_NAME_LEN 32
#define BT_MAX_PRINTED_MAC_LEN 20
typedef struct {
  char name[BT_MAX_NAME_LEN];
  uint8_t mac[6];
  int8_t rssi;
} ScanItem_t;


#define RPC_UART_BAUDRATE 921600
#define RPC_MAGIC 0xA5
#define RPC_VERSION 0x01
#define RPC_MAX_PAYLOAD 1024

enum RpcType : uint8_t {
  RPC_CMD = 0x01,
  RPC_RESP = 0x02,
  RPC_EVENT = 0x03,
  RPC_PING = 0x04,
  RPC_PONG = 0x05
};

enum RpcOpcode : uint8_t {
  RPC_SET_FOLDER = 0x10,
  RPC_SET_TRACK = 0x11,
  RPC_SET_PLAYBACK = 0x12,

  RPC_GET_FOLDER_IMAGE_HEADER = 0x40,
  RPC_GET_FOLDER_IMAGE_CHUNK = 0x41,
  RPC_GET_FOLDER_IMAGE_END = 0x42,

  RPC_BT_START_DISCOVERY = 0x50,
  RPC_BT_SAVE_TO_CONNECT = 0x51,
  RPC_BT_CLEAR_PAIRED = 0x52,
  RPC_BT_GET_SCAN_LIST = 0x53,
  RPC_BT_END_SCAN = 0x54,

  RPC_EVT_STATE_CHANGED = 0x80
};

typedef enum : uint8_t {
  BT_OFF = 0,
  BT_NO_DEVICE,
  BT_IDLE,
  BT_DISCOVERING,
  BT_CONNECTING,
  BT_CONNECTED,
  BT_ERROR
} BtState_e;

typedef enum : uint8_t {
  PB_STOPPED = 0,
  PB_PLAYING,
} PlaybackState_e;

#pragma pack(push, 1)
typedef struct {
  BtState_e bt;
  PlaybackState_e playback;
  bool sd_mounted;
  uint16_t currentFolder;
} PlayerState_t;

struct  RpcHeader {
  uint8_t magic;
  uint8_t version;
  uint16_t length;
  uint16_t seq;
  uint8_t type;
  uint8_t opcode;
};

typedef enum : uint8_t{
  _ack = 0,
  _imageChunk,
  _btConnect,
  _btScan
} PacketType_e;

struct ExecResult_t {
  uint8_t ok;
};
struct ImageChunk_t {
  uint32_t offset;
  uint32_t size;
};
struct Connect_t {
  uint8_t mac[6];
};

typedef struct {
  PacketType_e type;
  union {
    ExecResult_t isok;
    ImageChunk_t chunk;
    Connect_t bt;
    ScanItem_t scan;
    uint8_t raw[RPC_MAX_PAYLOAD];
  };
} Packet_t;
#pragma pack(pop)


enum RxState : uint8_t {
  RX_WAIT_MAGIC,
  RX_HEADER,
  RX_PAYLOAD,
  RX_CRC
};

struct RpcRxContext {
  RxState state = RX_WAIT_MAGIC;
  RpcHeader hdr;
  uint8_t payload[RPC_MAX_PAYLOAD];
  uint16_t payload_pos = 0;
  uint8_t hdr_pos = 0;
  uint8_t crc_pos = 0;
  uint8_t crc_buf[2];

  uint64_t payload_deadline = 0;
};

void rpc_init(void);
void rpc_task(void *arg);
bool rpc_intercom_is_active(void);

void rpc_set_new_folder(int8_t dir);
void rpc_set_new_track(int8_t dir);

void rpc_set_album_begin(void);
void rpc_request_album_chunk(uint16_t offset, uint16_t len);
void rpc_set_album_end(void);

void rpc_start_bt_scan(void);
void rpc_get_bt_scan_list(void);
void rpc_save_bt_item(uint8_t * mac);
void rpc_delete_bonding(void);
void rpc_set_bt_end_scan(void);

void ui_change_image(const uint16_t *new_img);
void ui_add_bt_item(ScanItem_t item);
#endif
