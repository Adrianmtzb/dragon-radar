#include "mic.h"
#include <Wire.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "hw.h"
#include "sound.h"

#ifndef MIC_I2C_ADDR
#define MIC_I2C_ADDR 0x40
#endif
#ifndef MIC_DEFAULT_GAIN
#define MIC_DEFAULT_GAIN 4  // 12 dB: the speaker is a few centimetres from the microphones
#endif
#define MIC_MAX_MS 4000

static bool present = false;
static int gainStep = MIC_DEFAULT_GAIN;
static int16_t *buf = nullptr;
static volatile uint32_t wantFrames = 0, gotFrames = 0;
static volatile bool started = false, running = false, finished = false;
static TaskHandle_t task = nullptr;

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MIC_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static int rd(uint8_t reg) {
  Wire.beginTransmission(MIC_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)MIC_I2C_ADDR, 1) != 1) return -1;
  return Wire.read();
}

static bool upd(uint8_t reg, uint8_t mask, uint8_t val) {
  int v = rd(reg);
  return v >= 0 && wr(reg, (uint8_t)((v & ~mask) | (val & mask)));
}

// Espressif's es7210 driver sequence (as in Waveshare's ES7210 example), slave mode, MIC1 and MIC2,
// 16-bit standard I2S, MCLK = 256 x fs (the clock divider row of 12.288 MHz / 48 kHz, which is the
// same ratio at any rate).
bool micBegin() {
  if (!hwI2cBegin() || !soundSampleRate()) return false;
  if (rd(0x00) < 0) {
    Serial.println("[mic] ES7210 not found");
    return false;
  }
  bool ok = wr(0x00, 0xFF) && wr(0x00, 0x41);               // reset
  ok = ok && wr(0x01, 0x1F);                                 // clocks off while configuring
  ok = ok && wr(0x09, 0x30) && wr(0x0A, 0x30);               // chip and power-up state cycles
  ok = ok && wr(0x40, 0xC3);                                 // analog: vdda 3.3 V, VMID 5 kOhm start
  ok = ok && wr(0x41, 0x70) && wr(0x42, 0x70);               // mic bias 2.87 V
  ok = ok && wr(0x07, 0x20) && wr(0x02, 0xC1);               // OSR, ADC divider 1 with DLL and doubler
  ok = ok && wr(0x04, 0x01) && wr(0x05, 0x00);               // LRCK = MCLK / 256
  for (uint8_t r = 0x43; r <= 0x46; r++) ok = ok && upd(r, 0x10, 0x00);
  ok = ok && wr(0x4B, 0xFF) && wr(0x4C, 0xFF);               // every mic powered down...
  ok = ok && upd(0x01, 0x0B, 0x00) && wr(0x4B, 0x00);        // ...then MIC1/2 clocks and power on
  ok = ok && upd(0x43, 0x10, 0x10) && upd(0x44, 0x10, 0x10);  // MIC1/2 PGA on
  ok = ok && upd(0x11, 0xE0, 0x60) && upd(0x11, 0x03, 0x00);  // 16-bit, standard I2S
  ok = ok && wr(0x12, 0x00);                                 // ADC1/2 on SDOUT1
  ok = ok && wr(0x06, 0x00);                                 // power up
  ok = ok && wr(0x47, 0x00) && wr(0x48, 0x00);               // MIC1/2 power
  present = ok && micSetGain(gainStep);
  Serial.printf("[mic] ES7210 %s, gain %.1f dB\n", present ? "ready" : "init failed", gainStep * 3.0f - (gainStep > 11 ? 1.5f * (gainStep - 11) : 0));
  return present;
}

bool micPresent() { return present; }

bool micSetGain(int step) {
  if (step < 0 || step > 14) return false;
  gainStep = step;
  return upd(0x43, 0x0F, step) && upd(0x44, 0x0F, step);
}

int micGain() { return gainStep; }

static void recLoop(void *) {
  static int16_t chunk[2 * 240];
  // Drop what the I2S queue holds (up to 60 ms recorded before we were asked).
  for (int i = 0; i < 8; i++) soundInputRead(chunk, 240, 100);
  started = true;
  uint32_t n = 0;
  while (n < wantFrames) {
    uint32_t want = min<uint32_t>(240, wantFrames - n);
    size_t got = soundInputRead(buf + 2 * n, want, 200);
    if (!got) break;
    n += got;
    gotFrames = n;
  }
  running = false;
  finished = true;
  task = nullptr;
  vTaskDelete(nullptr);
}

bool micRecordStart(uint32_t ms) {
  if (!present || running || finished) return false;
  const uint32_t rate = soundSampleRate();
  ms = constrain(ms, 50u, (uint32_t)MIC_MAX_MS);
  wantFrames = rate * ms / 1000;
  if (!buf) buf = (int16_t *)heap_caps_malloc((size_t)rate * MIC_MAX_MS / 1000 * 4, MALLOC_CAP_SPIRAM);
  if (!buf) return false;
  gotFrames = 0;
  started = false;
  running = true;
  // Core 0, above the synth (3): the queue is only 60 ms deep and must not overflow.
  if (xTaskCreatePinnedToCore(recLoop, "mic", 3072, nullptr, 4, &task, 0) != pdPASS) {
    running = false;
    return false;
  }
  return true;
}

bool micRecordStarted() { return started; }
bool micRecording() { return running; }

void micLoop(Print &out) {
  if (!finished) return;
  finished = false;
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t *p = (const uint8_t *)buf;
  const size_t n = (size_t)gotFrames * 4;
  out.printf("REC %u 2 %u\n", (unsigned)soundSampleRate(), (unsigned)gotFrames);
  char line[77];
  uint8_t col = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? p[i + 1] : 0) << 8 | (i + 2 < n ? p[i + 2] : 0);
    line[col++] = B64[(v >> 18) & 63];
    line[col++] = B64[(v >> 12) & 63];
    line[col++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
    line[col++] = i + 2 < n ? B64[v & 63] : '=';
    if (col >= 76) {
      line[col] = 0;
      out.println(line);
      col = 0;
    }
  }
  if (col) {
    line[col] = 0;
    out.println(line);
  }
  out.println("ENDREC");
}
