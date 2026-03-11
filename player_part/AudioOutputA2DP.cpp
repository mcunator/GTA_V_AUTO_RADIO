/*
  AudioOutputA2DP
  Adds additional bufferspace to the output chain
  
  Copyright (C) 2017  Earle F. Philhower, III

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <Arduino.h>
#include "AudioOutputA2DP.h"
#include <BluetoothA2DPSource.h>
#include <math.h>

BluetoothA2DPSource a2dp_source;
int16_t buffer[A2DP_BUFFER_SIZE][2];
int readPtr = 0;
int writePtr = 0;

int32_t feedBT(Frame *frame, int32_t frame_count) {
  int framedata = 0;

  int diff = (A2DP_BUFFER_SIZE + writePtr - readPtr) % A2DP_BUFFER_SIZE;
  int needsWait = 0;

  while (diff < frame_count) {
    delay(5);
    diff = (A2DP_BUFFER_SIZE + writePtr - readPtr) % A2DP_BUFFER_SIZE;
    needsWait++;
    // No data for 20ms, feed silence
    if (needsWait >= 4) {
      frame[0].channel1 = 0;
      frame[0].channel2 = 0;
      return 1;
    }
  }

  for (int sample = 0; sample < frame_count; ++sample) {
    if (readPtr != writePtr) {
      frame[sample].channel1 = buffer[readPtr][0];
      frame[sample].channel2 = buffer[readPtr][1];
      readPtr = (readPtr + 1) % A2DP_BUFFER_SIZE;
      framedata++;
    } else {
      break;
    }
  }
  return frame_count;
}

BluetoothA2DPSource *AudioOutputA2DP::source() {
  return &a2dp_source;
}

AudioOutputA2DP::AudioOutputA2DP(const char *ssid) {
  // compose name with ssid + "player"
  char name[32];
  snprintf(name, 32, "%s", ssid);
  a2dp_source.set_local_name(name);
  a2dp_source.set_data_callback_in_frames(feedBT);
  a2dp_source.set_auto_reconnect(false);
  a2dp_source.set_pin_code("0000", ESP_BT_PIN_TYPE_FIXED);
  a2dp_source.set_ssp_enabled(true);
  a2dp_source.set_volume(127);
  A2DPNoVolumeControl *vc = new A2DPNoVolumeControl();
  a2dp_source.set_volume_control(vc);
}

AudioOutputA2DP::~AudioOutputA2DP() {
}

bool AudioOutputA2DP::begin() {
  samples = 0;
  filled = false;
  return true;
}

bool AudioOutputA2DP::SetGain(float f) {
  if (f > 1.0) f = 1.0;
  a2dp_source.set_volume(f * 100);
  return true;
}

bool AudioOutputA2DP::ConsumeSample(int16_t sample[2]) {
  // Now, do we have space for a new sample?
  int nextWritePtr = (writePtr + 1) % A2DP_BUFFER_SIZE;
  if (nextWritePtr == readPtr) {
    filled = true;
    return false;
  }
  buffer[writePtr][0] = sample[0];
  buffer[writePtr][1] = sample[1];
  writePtr = nextWritePtr;
  samples++;
  return true;
}



bool AudioOutputA2DP::stop() {
  return true;
}
