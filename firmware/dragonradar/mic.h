#pragma once
// The board's microphones for measuring its own speaker: an ES7210 ADC (I2C MIC_I2C_ADDR) on the
// data-in line of the same I2S bus as the ES8311 (MCLK 256 x fs, slave). MIC1 and MIC2 come out as
// the left and right slots of SDOUT1. The ES7210's MIC3 is an AEC tap of the codec output, but it
// is only on SDOUT2, which the board does not route.
//
// A recording runs in a task on core 0 into PSRAM while the loop keeps running, so the radar can
// play (and render) as usual; the loop dumps it over serial when it is done (micLoop):
//   "REC <rate> <channels> <frames>", base64 of the 16-bit little-endian frames in 76-char lines,
//   "ENDREC" (tools/record.py turns it into a WAV).
#include <Arduino.h>

bool micBegin();      // after soundBegin(): the I2S bus must be running
bool micPresent();
bool micSetGain(int step);  // 0-14 = 0 dB to 37.5 dB (ES7210 PGA); default MIC_DEFAULT_GAIN
int micGain();
// Starts a recording of `ms` milliseconds; false while one runs or without memory. The recorder
// first drops what the I2S queue holds, then sets micRecording() true: start the sound after that.
bool micRecordStart(uint32_t ms);
bool micRecordStarted();  // the recorder is capturing (the stale queue is flushed)
bool micRecording();      // started and not finished
void micLoop(Print &out);  // dumps a finished recording
