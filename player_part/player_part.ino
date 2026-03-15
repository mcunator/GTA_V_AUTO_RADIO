#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputA2DP.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_avrc_api.h"

#include "player_rpc.h"
#include "SPIFFS.h"

AudioGeneratorWAV *waw;
AudioFileSourceSD *file;
AudioOutputA2DP *a2dp;

#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18
#define SD_CS 5

#define MAX_FOLDERS 20
#define NAME_LEN 64
#define PATH_LEN 128

#define DEFAULT_BUFFESR_SIZE (4096)
typedef struct {
  char path[PATH_LEN];
  uint32_t file_size;
} Track;

typedef struct {
  char image[PATH_LEN];
  Track track;
} Folder;

typedef struct {
  uint32_t time;
  uint32_t folder;
} SavedState_t;

#define TIME_STR    "%02d:%02d:%02d"
#define TIME_FMT(t)     t / 3600, (t % 3600) / 60, t % 60

static Folder folders[MAX_FOLDERS] = { 0 };
static uint32_t startPlayingTime = 0;
static int32_t additionalTimeOffset = 0;
static SavedState_t lastSaved = {0};

QueueHandle_t mp3EventQueue;
QueueHandle_t mp3CmdQueue;

typedef enum {
  EVT_TRACK_FINISHED = 1,
  EVT_AVRCP_PLAY,
  EVT_AVRCP_PAUSE,
  EVT_AVRCP_STOP,
  EVT_AVRCP_NEXT_TRACK,
  EVT_AVRCP_PREV_TRACK,
  EVT_AVRCP_NEXT_FOLDER,
  EVT_AVRCP_PREV_FOLDER,
} AudioEventType;

typedef enum {
  MP3_STOP = 1,
  MP3_PLAY,
  MP3_PAUSE,
} MP3CmdType_e;


typedef struct {
  AudioEventType type;
  uint16_t folder;
} AudioEvent;

typedef struct {
  MP3CmdType_e type;
  uint16_t folder;
} MP3Cmd_t;

uint16_t folderCount = 0;
uint16_t currentFolder = 0;
bool connected = false;
bool scanIsActive = false;
bool isPlaying = false;

static bool sdMounted = false;
void sendTrackFinish() {
  AudioEvent ev;
  ev.type = EVT_TRACK_FINISHED;
  ev.folder = currentFolder;
  xQueueSend(mp3EventQueue, &ev, 0);
}
void sendTrackStop() {
  AudioEvent ev;
  ev.type = EVT_AVRCP_STOP;
  ev.folder = currentFolder;
  xQueueSend(mp3EventQueue, &ev, 0);
}
void connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
  Serial.print("esp_a2d_connection_state_t ");
  Serial.println(a2dp->source()->to_str(state));
  uint8_t oldConnectState = connected;
  connected = ESP_A2D_CONNECTION_STATE_CONNECTED == state;
  if (connected) {
    sendTrackFinish();
    a2dp->source()->cancel_discovery();
  } else if (oldConnectState) {
    sendTrackStop();
  }
}

// ------------------------ AVRCP Callback ------------------------
void button_handler(uint8_t cmd, bool state) {
  if (state) return;  // реагируем только на нажатие
  AudioEvent ev;
  ev.folder = currentFolder;

  switch (cmd) {
    case ESP_AVRC_PT_CMD_PLAY: ev.type = EVT_AVRCP_PLAY; break;
    case ESP_AVRC_PT_CMD_PAUSE: ev.type = EVT_AVRCP_PAUSE; break;
    case ESP_AVRC_PT_CMD_FORWARD: ev.type = EVT_AVRCP_NEXT_FOLDER; break;
    case ESP_AVRC_PT_CMD_BACKWARD: ev.type = EVT_AVRCP_PREV_FOLDER; break;
    case ESP_AVRC_PT_CMD_STOP: ev.type = EVT_AVRCP_STOP; break;
    case ESP_AVRC_PT_CMD_REWIND: ev.type = EVT_AVRCP_PREV_TRACK; break;
    case ESP_AVRC_PT_CMD_FAST_FORWARD: ev.type = EVT_AVRCP_NEXT_TRACK; break;
    default: return;
  }

  xQueueSend(mp3EventQueue, &ev, 0);
}
static uint32_t timeZero = 0;
uint32_t nowTime() {
  return millis() / 1000 + timeZero;
}

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size) {
  if (bda == NULL || str == NULL || size < 18) {
    return NULL;
  }
  uint8_t *p = bda;
  sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
          p[0], p[1], p[2], p[3], p[4], p[5]);
  return str;
}

static uint8_t foundedDevices = 0;
static ScanItem_t scanList[MAX_SCAN_ITEMS] = { 0 };
void bt_get_scan_list(uint8_t *listSize, ScanItem_t *list) {
  *listSize = foundedDevices;
  for (int i = 0; i < foundedDevices; i++) {
    list[i] = scanList[i];
  }
}

void bt_set_scan_state(uint8_t state) {
  scanIsActive = state;
  if (scanIsActive) {
    foundedDevices = 0;
    memset(scanList, 0, sizeof(scanList));
  }
}
static uint8_t macToConnect[6] = { 0 };
void bt_set_to_connect(uint8_t *mac) {
  memcpy(macToConnect, mac, 6);
}

bool ssid_callback(const char *ssid, esp_bd_addr_t address, int rssi) {
  bool result = false;
  char mac[20] = { 0 };
  bda2str(address, mac, 20);
  Serial.print("SSID ");
  Serial.print(ssid == 0 ? "NO_SSID" : ssid);
  Serial.print(" MAC ");
  Serial.print(mac);
  Serial.print(" RSSI ");
  Serial.println(rssi);

  bool exist = false;
  for (int i = 0; i < MAX_SCAN_ITEMS; i++) {
    if (scanList[i].mac[0] != 0 && memcmp(scanList[i].mac, address, 6) == 0) {
      exist = true;
      scanList[i].rssi = rssi;
      break;
    }
  }
  if (!exist) {
    if (strlen(ssid) == 0) {
      strcpy(scanList[foundedDevices].name, mac);
    } else {
      strcpy(scanList[foundedDevices].name, ssid);
    }
    memcpy(scanList[foundedDevices].mac, address, 6);
    scanList[foundedDevices].rssi = rssi;
    foundedDevices++;
    foundedDevices %= MAX_SCAN_ITEMS;
  }

  if (!scanIsActive) {
    int dev_num = esp_bt_gap_get_bond_device_num();
    Serial.println(dev_num);
    if (dev_num != ESP_FAIL && dev_num > 0) {
      esp_bd_addr_t *dev_list = (esp_bd_addr_t *)malloc(dev_num * sizeof(esp_bd_addr_t));
      if (dev_list) {
        memset((uint8_t *)dev_list, 0, dev_num * sizeof(esp_bd_addr_t));
        if (esp_bt_gap_get_bond_device_list(&dev_num, dev_list) == ESP_OK) {
          for (int i = 0; i < dev_num; i++) {
            if (memcmp(dev_list[i], address, 6) == 0) {
              result = true;
              break;
            }
          }
        }
        free(dev_list);
      }
    }
    if (!result) {
      if (memcmp(macToConnect, address, 6) == 0) {
        result = true;
      }
    }
  }
  return result;
}

void discovery_mode_callback(esp_bt_gap_discovery_state_t discoveryMode) {
  Serial.print("discoveryMode ");
  Serial.println(discoveryMode);
  // The library sets is_end=true via cancel_discovery() so it won't auto-restart inquiry.
  // When a user scan is active we need to keep rescanning until the UI stops it.
  if (discoveryMode == ESP_BT_GAP_DISCOVERY_STOPPED && scanIsActive) {
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  }
}

bool isMP3(String name) {
  return name.endsWith(".wav");
}
bool isImage(String name) {
  return name.endsWith(".cover");
}

void scanSD() {
  File root = SD.open("/");
  if (!root) return;
  folderCount = 0;
  while (true) {
    File dir = root.openNextFile();
    if (!dir) break;
    if (!dir.isDirectory()) continue;
    Folder &f = folders[folderCount];
    f.image[0] = 0;
    while (true) {
      File file = dir.openNextFile();
      if (!file) break;
      if (file.isDirectory()) {

      } else {
        String name = file.name();
        if (isMP3(name)) {
          f.track.file_size = file.size();
          strcpy(f.track.path, file.path());
        }
        if (isImage(name) && f.image[0] == 0) {
          strcpy(f.image, file.path());
        }
      }
      file.close();
    }
    if (f.track.file_size != 0) {
      folderCount++;
    }
    dir.close();
  }
}
static uint8_t precachedImage[MAX_IMAGE_SIZE] = { 0 };
uint8_t *getCurrentAlbumFile(void) {
  return precachedImage;
}

void printDB() {
  for (int i = 0; i < folderCount; i++) {
    Serial.printf("Folder %d: \"%s\" \"%s\"\n",
                  i, folders[i].track.path, (folders[i].image[0] ? folders[i].image : "NO COVER"));
  }
}

void mp3Task(void *pvParameters) {
  MP3Cmd_t cmd;
  uint8_t noPlaying = 0;
  uint16_t lastFolder = 10000;
  while (1) {
    if (waw) {
      while (xQueueReceive(mp3CmdQueue, &cmd, 0) == pdPASS) {
        switch (cmd.type) {
          case MP3_PLAY:
            {
              if (folders[cmd.folder].track.file_size > 0) {
                Track *t = &folders[cmd.folder].track;
                if (waw->isRunning()) waw->stop();
                if (!file) {
                  file = new AudioFileSourceSD(t->path);
                } else {
                  file->close();
                }
                if (cmd.folder != lastFolder) {
                  lastFolder = cmd.folder;
                  File image = SD.open(folders[cmd.folder].image);
                  memset(precachedImage, 0, sizeof(precachedImage));
                  if (image) {
                    uint16_t color[480];
                    int size = image.size() / 2;
                    int imIndx = 0;
                    for (int i = 0; i < size;) {
                      int readed = image.readBytes((char *)&color, sizeof(color));
                      for (int j = 0; j < readed / 2; j++, i++) {
                        precachedImage[i / 8] |= color[j] == 0xFFFF ? (1 << (i % 8)) : 0;
                      }
                    }
                    image.close();
                  }
                }
                file->open(t->path);
                file->setBuffetSize(DEFAULT_BUFFESR_SIZE);
                const uint32_t bytes_per_sec = 176400;
                uint32_t elapsed_s = nowTime() + additionalTimeOffset;
                uint32_t duration_s = t->file_size / bytes_per_sec;
                uint32_t play_s = elapsed_s % duration_s;
                uint32_t offset = bytes_per_sec * play_s;
                Serial.printf("Track path %s offset %d " TIME_STR " " TIME_STR "\n", t->path, offset / 1024, TIME_FMT(play_s), TIME_FMT(duration_s));

                waw->begin(file, a2dp);
                file->seek(offset, SEEK_SET);
                if (!isPlaying) {
                  isPlaying = true;
                }
              }
            }
            break;

          case MP3_PAUSE:
          case MP3_STOP:
            if (waw->isRunning()) {
              waw->stop();
              if (isPlaying) {
                isPlaying = false;
              }
            }
            break;
        }
      }
      if (waw->isRunning() && waw->loop()) {
        noPlaying = 0;
      } else {
        noPlaying++;
        if (isPlaying && noPlaying > 5) {
          isPlaying = false;
          sendTrackFinish();
        }
        delay(100);  // небольшая пауза
      }
    }
    delay(2);
  }
}
void readFile(fs::FS &fs, const char *path, SavedState_t *s) {
  File file = fs.open(path);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }
  if(file.readBytes((char *)s, sizeof(SavedState_t)) != sizeof(SavedState_t)) {
    memset(s, 0, sizeof(SavedState_t));
  }
  file.close();
}

void writeFile(fs::FS &fs, const char *path, SavedState_t s) {
  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  if (file.write((uint8_t *)&s, sizeof(s))) {
     Serial.println("Success writing");
  } else {
    Serial.println("Write failed");
  }
  file.close();
}

void setup() {
  Serial.begin(921600);
  Serial.println("ESP32 MP3 -> A2DP start");
  rpc_init();

  // --- SD ---
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  SPI.setFrequency(50000000);

  if (!sdMounted) {
    folderCount = 0;
    if (SD.begin(SD_CS)) {
      sdMounted = true;
      Serial.println("SD mounted");
      scanSD();
      printDB();
    }
  }

  if (SPIFFS.begin(true)) {
    readFile(SPIFFS, "/time.txt", &lastSaved);
    lastSaved.time = lastSaved.time % (24*3600);
    timeZero = lastSaved.time;
    Serial.printf("Saved_time: " TIME_STR "\n", TIME_FMT(lastSaved.time));
    currentFolder = lastSaved.folder < folderCount ? lastSaved.folder : 0;
  }
  esp_err_t ret;
  ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  Serial.printf("mem_release BLE: %s\n", esp_err_to_name(ret));


  // --- A2DP ---
  a2dp = new AudioOutputA2DP("GTA_V_Radio");
  if (a2dp->begin() == false) {
    Serial.println("NO MEMORY");
    return;
  }

  a2dp->source()->set_default_bt_mode(ESP_BT_MODE_CLASSIC_BT);
  a2dp->source()->set_local_name("GTA_V_Radio");
  a2dp->source()->set_avrc_passthru_command_callback(button_handler);
  a2dp->source()->set_ssid_callback(ssid_callback);
  a2dp->source()->set_discovery_mode_callback(discovery_mode_callback);
  a2dp->source()->set_on_connection_state_changed(connection_state_changed);

  a2dp->source()->start();
  a2dp->source()->set_local_name("GTA_V_Radio");

  // --- MP3 ---
  waw = new AudioGeneratorWAV();
  waw->SetBufferSize(DEFAULT_BUFFESR_SIZE);

  mp3EventQueue = xQueueCreate(10, sizeof(AudioEvent));
  mp3CmdQueue = xQueueCreate(10, sizeof(MP3Cmd_t));
  xTaskCreatePinnedToCore(mp3Task, "MP3", 4096, NULL, 5, NULL, 1);
}

void playTrack(uint16_t index) {
  if (index >= folderCount) return;
  currentFolder = index;

  MP3Cmd_t cmd;
  cmd.type = MP3_PLAY;
  cmd.folder = index;
  xQueueSend(mp3CmdQueue, &cmd, 0);
}

void pausePlayback() {
  MP3Cmd_t cmd;
  cmd.type = MP3_PAUSE;
  cmd.folder = currentFolder;
  xQueueSend(mp3CmdQueue, &cmd, 0);
}

static uint32_t lastFlip = 0;
static uint8_t isScannable = 0;
void connectFlip(void) {
  if (scanIsActive) return;  // don't touch BT mode while user scan is in progress

  if (millis() - lastFlip > 10000 && !a2dp->source()->is_connected()) {
    lastFlip = millis();
    if (!isScannable) {
      // discovery mode → scannable: become visible for incoming pairing
      isScannable = 1;
      Serial.println("Set scannable");
      a2dp->source()->set_auto_reconnect(false);
      a2dp->source()->cancel_discovery();
      a2dp->source()->set_connectable(true);
      a2dp->source()->set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
    } else {
      // scannable mode → discovery: try to connect to known devices
      isScannable = 0;
      Serial.println("Set discovery");
      a2dp->source()->set_connectable(false);
      a2dp->source()->set_discoverability(ESP_BT_NON_DISCOVERABLE);
      a2dp->source()->set_auto_reconnect(true);
      int dev_num = esp_bt_gap_get_bond_device_num();
      if (dev_num != ESP_FAIL && dev_num > 0) {
        esp_bd_addr_t *dev_list = (esp_bd_addr_t *)malloc(dev_num * sizeof(esp_bd_addr_t));
        if (dev_list) {
          memset((uint8_t *)dev_list, 0, dev_num * sizeof(esp_bd_addr_t));
          if (esp_bt_gap_get_bond_device_list(&dev_num, dev_list) == ESP_OK) {
            a2dp->source()->connect_to(dev_list[0]);
          }
          free(dev_list);
        }
      } else {
        // no bonded devices — start inquiry to discover any available device
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
      }
    }
  }
}
#define TRACK_STEP_TIME 120
void loop() {
  AudioEvent ev;
  if (isPlaying) {
    if (nowTime() + additionalTimeOffset - lastSaved.time > TRACK_STEP_TIME || lastSaved.folder != currentFolder) {
      lastSaved.time = nowTime() + additionalTimeOffset;
      lastSaved.folder = currentFolder;
      writeFile(SPIFFS, "/time.txt", lastSaved);
    }
  }

  if(xQueueReceive(mp3EventQueue, &ev, 0) == pdPASS) {
    switch (ev.type) {
      case EVT_TRACK_FINISHED:
        if (startPlayingTime == 0) {
          startPlayingTime = nowTime();
        }
        Serial.printf("Track %d finished\n", ev.folder);
        playTrack(currentFolder);
        break;
      case EVT_AVRCP_PLAY:
        playTrack(currentFolder);
        break;
      case EVT_AVRCP_PAUSE:
      case EVT_AVRCP_STOP:
        pausePlayback();
        break;
      case EVT_AVRCP_NEXT_TRACK:
        additionalTimeOffset += TRACK_STEP_TIME;
        playTrack(currentFolder);
        break;

      case EVT_AVRCP_PREV_TRACK:
        if (additionalTimeOffset - TRACK_STEP_TIME + startPlayingTime > 0) {
          additionalTimeOffset -= TRACK_STEP_TIME;
        }
        playTrack(currentFolder);       
        break;
      case EVT_AVRCP_NEXT_FOLDER:
        playTrack((currentFolder + 1) % folderCount);
        break;
      case EVT_AVRCP_PREV_FOLDER:
        playTrack((currentFolder + folderCount - 1) % folderCount);
        break;
    }
  }
  connectFlip();
  rpc_task(a2dp->source());
  delay(10);
}
