/*
  AudioOutputA2DP
  Base class for an A2DP output port
  
  Copyright (C) 2023, Thomas BOHL

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

#pragma once

#include "AudioOutput.h"
#include <BluetoothA2DPSource.h>
#include "AudioTools.h"
#define A2DP_BUFFER_SIZE (2048)

class AudioOutputA2DP : public AudioOutput
{
  public:
    AudioOutputA2DP(const char* ssid);
    virtual ~AudioOutputA2DP() override;
    virtual bool begin() override;
    virtual bool SetGain(float f) override;
    virtual bool ConsumeSample(int16_t sample[2]) override;
    virtual bool stop() override;

    int GetSamples() { return samples; }
    BluetoothA2DPSource * source();
  protected:
    int samples;
    bool filled;

};