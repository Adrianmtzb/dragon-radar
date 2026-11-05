#include "sound.h"
#include <math.h>
#include "led.h"

// LEDC channel 2 (timer 1): the backlight owns channel 0 / timer 0, and channels 2n and 2n+1 share
// a timer, so the buzzer must not sit next to it (see docs/hardware.md).
static const uint8_t BUZZER_CHANNEL = 2;
static const uint8_t BUZZER_RES_BITS = 10;

// Pins from the board header. A board with no buzzer at all may leave BUZZER_PINS undefined.
#ifdef BUZZER_PINS
static constexpr uint8_t PINS[] = BUZZER_PINS;
static constexpr int PIN_COUNT = sizeof(PINS);
static constexpr bool listHas(const uint8_t *p, int n, int pin) { return n > 0 && (p[0] == pin || listHas(p + 1, n - 1, pin)); }
#if HAS_BUZZER
static_assert(listHas(PINS, PIN_COUNT, PIN_BUZZER), "BUZZER_PINS must include PIN_BUZZER");
#endif
#ifdef BUZZER_DEFAULT_PIN
static_assert(listHas(PINS, PIN_COUNT, BUZZER_DEFAULT_PIN), "BUZZER_PINS must include BUZZER_DEFAULT_PIN");
#endif
#else
#if HAS_BUZZER
#error "HAS_BUZZER needs BUZZER_PINS in the board header"
#endif
static const uint8_t *const PINS = nullptr;
static constexpr int PIN_COUNT = 0;
#endif

#if defined(BUZZER_DEFAULT_PIN)
#define BUZZER_PIN_DEFAULT BUZZER_DEFAULT_PIN
#elif HAS_BUZZER
#define BUZZER_PIN_DEFAULT PIN_BUZZER
#else
#define BUZZER_PIN_DEFAULT (PIN_COUNT ? PINS[0] : -1)
#endif

#if defined(AUDIO_ES8311) && AUDIO_ES8311
#define SOUND_I2S 1
#else
#define SOUND_I2S 0
#endif

static Tone seq[SOUND_MAX_TONES];
static int seqVolume = -1;  // one-off volume of the current sequence, -1 = cfg.volume
static uint8_t curWave = SOUND_DEFAULT_WAVE;
static int seqLen = 0, seqPos = 0, repeatLeft = 0;
static SoundPriority prio = SND_SFX;
static bool playing = false;
static uint32_t toneStart = 0, toneEnd = 0, silenceAt = 0;
static uint32_t remainingMs = 0;
static uint32_t reqCounter = 0;  // soundPlaySfxFrom() requests so far

static SoundConfig cfg = soundDefaults();
static bool begun = false;
static int activePin = -1;  // buzzer pin attached to LEDC, -1 when the buzzer is off

SoundConfig soundDefaults() {
  // The on-board buzzer starts enabled; an external one stays off until someone enables it.
  return {(bool)HAS_BUZZER, (int8_t)BUZZER_PIN_DEFAULT, SOUND_DEFAULT_VOLUME, true};
}

const SoundConfig &soundConfig() { return cfg; }
bool soundAvailable() { return PIN_COUNT > 0; }
bool soundBuiltin() { return HAS_BUZZER; }
bool soundHasBuzzer() { return activePin >= 0; }
int soundPinCount() { return PIN_COUNT; }
int soundPinAt(int i) { return i >= 0 && i < PIN_COUNT ? PINS[i] : -1; }

bool soundPinAllowed(int pin) {
  for (int i = 0; i < PIN_COUNT; i++)
    if (PINS[i] == pin) return true;
  return false;
}

bool soundUsesPin(int pin) { return pin >= 0 && cfg.enabled && cfg.pin == pin; }


#if SOUND_I2S
// ------------------------------------------------------------------ I2S codec (ES8311)
// A FreeRTOS task renders the current tone into the I2S DMA (blocking writes there, never in the
// loop). The loop's sequencer only changes the target frequency, waveform and amplitude; the task
// ramps the amplitude over ~2 ms so tones start and stop without clicks, and drives the speaker
// amplifier: on (with a short settle) when a tone starts, off after SOUND_PA_OFF_MS of silence.
#include <ESP_I2S.h>
#include <Wire.h>
#include "hw.h"

// 48 kHz, MCLK 256 x fs = 12.288 MHz (a row of the ES8311 and ES7210 clock tables). At 24 kHz the
// harmonics of the radar's 4260 Hz tone folded back into the audible range as inharmonic tones.
#ifndef SOUND_RATE
#define SOUND_RATE 48000
#endif
// The amplifier pops when it switches. It stays on through the radar's beep loop (beeps come every
// 0.16-1.4 s) and goes off only after this much silence.
#define SOUND_PA_OFF_MS 4000
#define SOUND_PA_SETTLE_MS 20  // after switching it on, before the first tone
#define SOUND_BLOCK 480        // frames per write: 10 ms
static I2SClass i2s;
static TaskHandle_t synthTask = nullptr;
static bool i2sOk = false, codecOk = false, i2sTried = false;
static volatile bool paOn = false;
static volatile uint32_t blocksWritten = 0, shortWrites = 0;
// Sine table with linear interpolation (1024 steps: spurs below -90 dB).
static const int SINE_BITS = 10;
static int16_t sineTable[(1 << SINE_BITS) + 1];

static inline int32_t sineAt(uint32_t phase) {
  const uint32_t i = phase >> (32 - SINE_BITS), frac = (phase >> (32 - SINE_BITS - 15)) & 0x7FFF;
  const int32_t a = sineTable[i], b = sineTable[i + 1];
  return a + (((b - a) * (int32_t)frac) >> 15);
}

// Band-limited triangle: its odd harmonics (amplitude 1/k^2, alternating sign) up to 0.45 fs, so
// nothing folds back. Peak 1.0 for the full series (8 / pi^2 normalisation).
static const int TRI_MAX_TERMS = 8;
static int triTerms = 1;
static int32_t triGain[TRI_MAX_TERMS];  // Q15 coefficient of harmonic 2n+1

static void triSetup(uint16_t hz) {
  triTerms = 0;
  for (int n = 0; n < TRI_MAX_TERMS; n++) {
    const int k = 2 * n + 1;
    if ((uint32_t)k * hz > SOUND_RATE * 45 / 100) break;
    triGain[n] = (int32_t)lroundf(32767.0f * 8.0f / ((float)M_PI * (float)M_PI) / (k * k) * (n & 1 ? -1 : 1));
    triTerms = n + 1;
  }
  if (!triTerms) {
    triGain[0] = 26557;  // 8 / pi^2: a plain sine when even the fundamental is near Nyquist
    triTerms = 1;
  }
}

static inline int32_t triAt(uint32_t phase) {
  int32_t acc = 0;
  for (int n = 0; n < triTerms; n++) acc += (sineAt(phase * (uint32_t)(2 * n + 1)) * triGain[n]) >> 15;
  return acc;
}

static bool codecWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(CODEC_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool codecRead(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(CODEC_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)CODEC_I2C_ADDR, 1) != 1) return false;
  val = Wire.read();
  return true;
}

// DAC-only bring-up, the register sequence of Espressif's es8311 driver (as in Waveshare's example)
// for slave mode, MCLK from its pin at 256 x fs, 16-bit I2S, fs = SOUND_RATE (the 12.288 MHz /
// 48 kHz row of its clock table: pre-divider 1, multiplier 1, OSR 16, LRCK = MCLK / 256).
static bool codecBegin() {
  if (!hwI2cBegin()) return false;
  uint8_t id1 = 0, id2 = 0, v = 0;
  if (!codecRead(0xFD, id1) || !codecRead(0xFE, id2)) {
    Serial.println("[sound] ES8311 not found");
    return false;
  }
  bool ok = codecWrite(0x00, 0x1F) && codecWrite(0x00, 0x00) && codecWrite(0x00, 0x80);  // reset, power on, slave
  ok = ok && codecWrite(0x01, 0x3F);                                  // every clock on, MCLK from the pin
  ok = ok && codecRead(0x06, v) && codecWrite(0x06, (v & 0xC0) | 0x03);  // BCLK not inverted, divider 4
  ok = ok && codecRead(0x02, v) && codecWrite(0x02, v & 0x07);        // pre-divider 1, multiplier 1
  ok = ok && codecWrite(0x03, 0x10) && codecWrite(0x04, 0x10);        // single speed, ADC/DAC OSR 16
  ok = ok && codecWrite(0x05, 0x00);                                  // ADC and DAC clock dividers 1
  ok = ok && codecRead(0x07, v) && codecWrite(0x07, v & 0xC0) && codecWrite(0x08, 0xFF);  // LRCK = MCLK / 256
  ok = ok && codecWrite(0x09, 0x0C) && codecWrite(0x0A, 0x0C);        // 16-bit I2S in and out
  ok = ok && codecWrite(0x0D, 0x01) && codecWrite(0x0E, 0x02);        // analog power up
  ok = ok && codecWrite(0x12, 0x00) && codecWrite(0x13, 0x10);        // DAC on, output driver on
  ok = ok && codecWrite(0x1C, 0x6A) && codecWrite(0x37, 0x08);        // ADC DC offset cancel, DAC EQ bypass
  ok = ok && codecWrite(0x32, 0xBF);                                  // DAC volume 0 dB: loudness is set in software
  Serial.printf("[sound] ES8311 id %02X%02X %s\n", id1, id2, ok ? "ready" : "init failed");
  return ok;
}

static void paSet(bool on) {
  digitalWrite(PIN_PA_CTRL, on ? HIGH : LOW);
  paOn = on;
}

// Sequencer of the synth task. The loop hands a whole sequence over (synthPlay) and the task plays
// it with sample-exact timing, so tone lengths do not depend on how long the loop takes to come
// back (a full-screen redraw can hold it for 60 ms).
static Tone pendSeq[SOUND_MAX_TONES];  // written by the loop under the lock
static int pendLen = 0, pendRepeat = 1;
static int32_t pendAmp = 0;
static volatile uint32_t pendGen = 0, actGen = 0;
static volatile bool tPlaying = false;
static volatile uint8_t tWave = WAVE_SQUARE, tLastWave = WAVE_SQUARE;  // waveform playing / of the last tone
static volatile uint32_t tRemainMs = 0;
static portMUX_TYPE synthMux = portMUX_INITIALIZER_UNLOCKED;
static Tone actSeq[SOUND_MAX_TONES];   // the task's own copy

// Volume 0-100 to amplitude (the synth rule of shared/sounds.json on the radar): levelAt100 x
// 10^(-rangeDb x (1 - v/100)^shape / 20), 0 = silent. With 36 dB and 2.5, flatter at the top: volume
// 60 is -3.6 dB, 30 is -16 dB, 10 is -28 dB.
#ifndef SOUND_LEVEL_AT_100
#define SOUND_LEVEL_AT_100 0.95f
#endif
#ifndef SOUND_RANGE_DB
#define SOUND_RANGE_DB 36.0f
#endif
#ifndef SOUND_CURVE_SHAPE
#define SOUND_CURVE_SHAPE 2.5f
#endif
#ifndef SOUND_RAMP_MS
#define SOUND_RAMP_MS 2.0f
#endif

static int32_t ampFor(int volume) {
  if (volume <= 0) return 0;
  const float v = min(volume, 100) / 100.0f;
  const float db = -SOUND_RANGE_DB * powf(1 - v, SOUND_CURVE_SHAPE);
  return max<int32_t>(1, (int32_t)lroundf(32767.0f * SOUND_LEVEL_AT_100 * powf(10.0f, db / 20)));
}

// Start timing of tagged requests (soundPlaySfxFrom): the loop tags a request, the task notes when
// it took it and what stood before its first sample (soundLastStart). A dry run goes through its
// own channel, so it never replaces or stops what is playing and never touches the amplifier.
// ESP_I2S's DMA: 6 descriptors x 240 frames. While the amplifier is on the task keeps writing
// (silence when idle), so the queue is full; when it sleeps the queue runs empty.
#define SOUND_DMA_QUEUE_MS (6 * 240 * 1000 / SOUND_RATE)
#define SOUND_IDLE_QUEUE_MS (2 * 240 * 1000 / SOUND_RATE)  // up to two descriptors already on their way
static uint32_t tagNo = 0, tagLead = 0, tagDur = 0;  // the next synthPlay()'s tag, set by the loop
static uint32_t pendReqNo = 0, pendReqUs = 0, pendLead = 0, pendDur = 0;
static uint32_t dryReqNo = 0, dryReqUs = 0, dryLead = 0, dryDur = 0;
static volatile uint32_t dryGen = 0, dryAct = 0;
static uint32_t dryPaOffAt = 0;  // task side: when the modelled amplifier of the dry runs goes off
static SoundStartInfo lastStart = {};

static void noteStart(uint32_t no, bool dry, uint32_t reqUs, uint32_t lead, uint32_t dur, bool pa) {
  SoundStartInfo i;
  i.request = no;
  i.dryRun = dry;
  i.requestUs = reqUs;
  i.takenUs = micros();
  i.paWasOn = pa;
  i.settleMs = pa ? 0 : SOUND_PA_SETTLE_MS;
  i.queueMs = pa ? SOUND_DMA_QUEUE_MS : SOUND_IDLE_QUEUE_MS;
  i.leadRestMs = lead;
  i.durationMs = dur;
  portENTER_CRITICAL(&synthMux);
  lastStart = i;
  portEXIT_CRITICAL(&synthMux);
}

static void synthDry(uint32_t no, uint32_t lead, uint32_t dur) {
  portENTER_CRITICAL(&synthMux);
  dryReqNo = no;
  dryReqUs = micros();
  dryLead = lead;
  dryDur = dur;
  dryGen = dryGen + 1;
  portEXIT_CRITICAL(&synthMux);
  if (synthTask) xTaskNotifyGive(synthTask);
}

// len 0 stops whatever is playing.
static void synthPlay(const Tone *tones, int len, int repeat, int volume) {
  portENTER_CRITICAL(&synthMux);
  pendReqNo = tagNo;
  pendReqUs = micros();
  pendLead = tagLead;
  pendDur = tagDur;
  tagNo = 0;
  pendLen = len;
  pendRepeat = repeat;
  pendAmp = ampFor(volume);
  if (len > 0) memcpy(pendSeq, tones, sizeof(Tone) * len);
  pendGen = pendGen + 1;
  portEXIT_CRITICAL(&synthMux);
  if (synthTask) xTaskNotifyGive(synthTask);
}

static void synthLoop(void *) {
  static int16_t buf[SOUND_BLOCK * 2];
  const int32_t RAMP = max(1, (int)(32767 / (SOUND_RATE * SOUND_RAMP_MS / 1000)));  // full scale in rampMs
  const uint32_t SPMS = SOUND_RATE / 1000;          // samples per millisecond
  uint32_t phase = 0, step = 0;
  int32_t amp = 0, target = 0, seqAmp = 0;
  uint8_t wave = WAVE_SQUARE;
  bool pa = false;
  uint32_t silentSince = 0;
  int len = 0, pos = 0, repeatLeft = 0;
  uint32_t toneLeft = 0, soundLeft = 0;  // samples left in the tone, and before its articulation gap
  uint32_t settle = 0;                   // samples of silence while the amplifier wakes up
  bool active = false;

  auto startTone = [&]() {
    const Tone &t = actSeq[pos];
    toneLeft = (uint32_t)t.ms * SPMS;
    uint32_t gap = t.legato ? 0 : min<uint32_t>(14, t.ms / 6);
    soundLeft = toneLeft - gap * SPMS;
    uint16_t hz = min<uint16_t>(t.hz, SOUND_RATE / 2 - 1);
    step = (uint32_t)((uint64_t)hz * 4294967296ULL / SOUND_RATE);
    wave = t.wave == WAVE_DEFAULT ? SOUND_DEFAULT_WAVE : t.wave;
    if (wave == WAVE_TRI && t.hz) triSetup(hz);
    if (t.hz) {
      tWave = wave;
      tLastWave = wave;
    }
    target = t.hz ? seqAmp : 0;
  };

  for (;;) {
    if (dryGen != dryAct) {  // a dry run: only when it would have started
      portENTER_CRITICAL(&synthMux);
      const uint32_t no = dryReqNo, us = dryReqUs, lead = dryLead, dur = dryDur;
      dryAct = dryGen;
      portEXIT_CRITICAL(&synthMux);
      // The amplifier as it would be had the dry runs played: on until SOUND_PA_OFF_MS after the
      // last one ended.
      const uint32_t t = millis();
      const bool vpa = pa || (dryPaOffAt && (int32_t)(t - dryPaOffAt) < 0);
      noteStart(no, true, us, lead, dur, vpa);
      dryPaOffAt = (t + (vpa ? 0 : SOUND_PA_SETTLE_MS) + dur + SOUND_PA_OFF_MS) | 1;
    }
    if (pendGen != actGen) {  // a new sequence (or a stop) from the loop
      portENTER_CRITICAL(&synthMux);
      const uint32_t no = pendReqNo, us = pendReqUs, lead = pendLead, dur = pendDur;
      len = pendLen;
      repeatLeft = max(1, pendRepeat) - 1;
      seqAmp = pendAmp;
      if (len > 0) memcpy(actSeq, pendSeq, sizeof(Tone) * len);
      actGen = pendGen;
      portEXIT_CRITICAL(&synthMux);
      if (no) noteStart(no, false, us, lead, dur, pa);
      pos = 0;
      active = len > 0 && seqAmp > 0;
      if (active) {
        uint32_t total = 0;
        for (int i = 0; i < len; i++) total += actSeq[i].ms;
        tRemainMs = total * (repeatLeft + 1);
        if (!pa) {
          paSet(pa = true);
          settle = SOUND_PA_SETTLE_MS * SPMS;  // the sequence clock starts once the amplifier is up
        }
        startTone();
      } else {
        target = 0;
        tRemainMs = 0;
      }
      tPlaying = active;
    }
    uint32_t now = millis();
    if (!active && amp == 0) {
      if (!silentSince) silentSince = now | 1;
      if (pa && (int32_t)(now - silentSince) >= SOUND_PA_OFF_MS) paSet(pa = false);
      if (!pa) {  // idle: sleep until synthPlay() has something
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        continue;
      }
    } else {
      silentSince = 0;
    }
    for (int i = 0; i < SOUND_BLOCK; i++) {
      int32_t want = 0;
      if (settle) {
        settle--;
      } else if (active) {
        want = soundLeft ? target : 0;
        if (soundLeft) soundLeft--;
        if (--toneLeft == 0) {
          uint16_t ms = actSeq[pos].ms;
          tRemainMs = tRemainMs > ms ? tRemainMs - ms : 0;
          if (++pos >= len) {
            if (repeatLeft-- > 0) pos = 0;
            else active = false;
          }
          if (active) startTone();
          else {
            target = 0;
            tPlaying = false;
            tRemainMs = 0;
          }
        }
      }
      if (amp < want) amp = min(want, amp + RAMP);
      else if (amp > want) amp = max(want, amp - RAMP);
      phase += step;
      int32_t sm = 0;
      if (amp) {
        if (wave == WAVE_SINE) sm = sineAt(phase);
        else if (wave == WAVE_TRI) sm = triAt(phase);
        else sm = (phase & 0x80000000u) ? -32767 : 32767;  // naive: only the 5 ms zoom clicks use it
      }
      int16_t v = (int16_t)(sm * amp / 32767);
      buf[2 * i] = buf[2 * i + 1] = v;  // the DAC is mono: same sample on both slots
    }
    if (i2s.write((const uint8_t *)buf, sizeof(buf)) != sizeof(buf)) shortWrites = shortWrites + 1;
    blocksWritten = blocksWritten + 1;
  }
}

static void i2sStart() {
  if (i2sTried) return;
  i2sTried = true;
  pinMode(PIN_PA_CTRL, OUTPUT);
  paSet(false);
  for (int i = 0; i <= (1 << SINE_BITS); i++)
    sineTable[i] = (int16_t)lroundf(32767.0f * sinf(i * 2.0f * (float)M_PI / (1 << SINE_BITS)));
  // I2S first: the codec needs MCLK running while it is configured.
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  i2sOk = i2s.begin(I2S_MODE_STD, SOUND_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  if (!i2sOk) {
    Serial.println("[sound] I2S init failed");
    return;
  }
  codecOk = codecBegin();
  if (codecOk) xTaskCreatePinnedToCore(synthLoop, "synth", 4096, nullptr, 3, &synthTask, 0);
}

#endif

// ------------------------------------------------------------------ output

static void output(uint16_t hz) {
  if (activePin < 0) return;
#if SOUND_I2S
  (void)hz;  // the synth task plays whole sequences (synthPlay)
  return;
#endif
  if (!hz || cfg.volume == 0) {
    ledcWrite(activePin, 0);
    return;
  }
  // Perceived loudness of a piezo grows roughly with the square of the duty (50% is the loudest).
  uint32_t maxDuty = 1u << (BUZZER_RES_BITS - 1);
  uint32_t duty = max<uint32_t>(1, maxDuty * cfg.volume * cfg.volume / 10000);
  ledcChangeFrequency(activePin, hz, BUZZER_RES_BITS);
  ledcWrite(activePin, duty);
}

static void apply() {
  int want = cfg.enabled ? cfg.pin : -1;
  if (want == activePin) return;
#if SOUND_I2S
  // The speaker sits behind the codec: "pin" is its I2S data line and only on or off matters.
  if (activePin >= 0) synthPlay(nullptr, 0, 1, 0);
  activePin = -1;
  if (want < 0) {
    Serial.println("[sound] speaker off");
    return;
  }
  i2sStart();
  if (codecOk) {
    activePin = want;
    Serial.printf("[sound] speaker on (I2S %d Hz, data GPIO %d)\n", SOUND_RATE, want);
  }
  return;
#endif
  if (activePin >= 0) {
    ledcWrite(activePin, 0);
    ledcDetach(activePin);
    pinMode(activePin, INPUT);
    Serial.printf("[sound] buzzer off GPIO %d\n", activePin);
  }
  activePin = -1;
  if (want < 0) return;
  if (ledcAttachChannel(want, 2000, BUZZER_RES_BITS, BUZZER_CHANNEL)) {
    activePin = want;
    ledcWrite(activePin, 0);
    Serial.printf("[sound] buzzer on GPIO %d\n", activePin);
  } else {
    Serial.printf("[sound] buzzer PWM attach failed on GPIO %d\n", want);
  }
}

bool soundConfigure(const SoundConfig &c, String &err) {
  if (c.volume > 100) {
    err = "volume must be 0-100";
    return false;
  }
  if (!soundPinAllowed(c.pin)) {
    err = PIN_COUNT ? "pin not allowed for the buzzer on this board" : "this board has no buzzer pins";
    return false;
  }
  if (c.enabled && ledUsesPin(c.pin)) {
    err = "that pin is used by the status LED, move or turn off the LED first";
    return false;
  }
  cfg = c;
  if (begun) {
    if (!cfg.enabled) soundStop();
    apply();
  }
  return true;
}

void soundBegin() {
  begun = true;
  if (!soundPinAllowed(cfg.pin)) cfg = soundDefaults();
  if (!soundAvailable()) Serial.println("[sound] no buzzer on this board");
  apply();
}

static void startTone(uint32_t now) {
  const Tone &t = seq[seqPos];
  curWave = t.wave;
  toneStart = now;
  toneEnd = now + t.ms;
  // Separate consecutive notes so repeated pitches are heard as two notes.
  uint32_t gap = t.legato ? 0 : min<uint32_t>(14, t.ms / 6);
  silenceAt = toneEnd - gap;
  output(t.hz);
}

static void startSequence(SoundPriority p, int repeat, int volume = -1) {
  prio = p;
  seqVolume = volume;
  repeatLeft = constrain(repeat, 1, 20) - 1;
  seqPos = 0;
  playing = seqLen > 0;
  remainingMs = soundDurationMs(seq, seqLen) * (repeatLeft + 1);
#if SOUND_I2S
  if (activePin < 0) playing = false;
  synthPlay(seq, playing ? seqLen : 0, repeatLeft + 1, seqVolume >= 0 ? seqVolume : cfg.volume);
  return;
#endif
  if (playing) startTone(millis());
  else output(0);
}

void soundLoop() {
#if SOUND_I2S
  // The task owns the timing; once it has taken the latest sequence, its flag is the truth.
  if (playing && actGen == pendGen && !tPlaying) playing = false;
  return;
#endif
  if (!playing) return;
  uint32_t now = millis();
  if ((int32_t)(now - silenceAt) >= 0 && (int32_t)(now - toneEnd) < 0) output(0);
  if ((int32_t)(now - toneEnd) < 0) return;
  remainingMs = remainingMs > seq[seqPos].ms ? remainingMs - seq[seqPos].ms : 0;
  if (++seqPos >= seqLen) {
    if (repeatLeft-- > 0) {
      seqPos = 0;
    } else {
      playing = false;
      output(0);
      return;
    }
  }
  startTone(now);
}

void soundStop() {
  playing = false;
  seqLen = 0;
  remainingMs = 0;
#if SOUND_I2S
  synthPlay(nullptr, 0, 1, 0);
#endif
  output(0);
}

bool soundPlaying() {
#if SOUND_I2S
  return playing && (actGen != pendGen || tPlaying);
#else
  return playing;
#endif
}

uint32_t soundRemainingMs() {
#if SOUND_I2S
  return soundPlaying() ? (actGen != pendGen ? remainingMs : tRemainMs) : 0;
#else
  return playing ? remainingMs : 0;
#endif
}

uint32_t soundDurationMs(const Tone *tones, int n) {
  uint32_t ms = 0;
  for (int i = 0; i < n; i++) ms += tones[i].ms;
  return ms;
}

// ------------------------------------------------------------------ parsing

static uint16_t noteHz(int semitoneFromC, int octave) {
  int midi = 12 * (octave + 1) + semitoneFromC;
  return (uint16_t)lroundf(440.0f * powf(2.0f, (midi - 69) / 12.0f));
}

static int letterSemitone(char c) {
  switch (c | 0x20) {
    case 'c': return 0;
    case 'd': return 2;
    case 'e': return 4;
    case 'f': return 5;
    case 'g': return 7;
    case 'a': return 9;
    case 'b': return 11;
  }
  return -1;
}

static bool validDuration(int d) { return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32 || d == 64; }

static uint16_t clampMs(float ms) { return (uint16_t)constrain((int)lroundf(ms), 1, 10000); }

int soundParseNotes(const char *notes, int tempo, Tone *out, int max, String &err) {
  if (!notes) {
    err = "missing notes";
    return -1;
  }
  tempo = constrain(tempo, 20, 400);
  float quarterMs = 60000.0f / tempo;
  int n = 0;
  uint8_t wave = WAVE_DEFAULT;
  const char *p = notes;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',' || *p == '|') p++;
    if (!*p) break;
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != ',' && *p != '|') p++;
    String tok(start, p - start);
    if (tok[0] == '~') {  // waveform switch for the tones that follow
      SoundWave w;
      if (!soundWaveFromName(tok.c_str() + 1, w)) {
        err = "bad waveform '" + tok + "': use ~square, ~sine or ~tri";
        return -1;
      }
      wave = w;
      continue;
    }
    if (n >= max) {
      err = "too many notes (max " + String(max) + ")";
      return -1;
    }
    Tone t = {0, 0, false, wave};
    int colon = tok.indexOf(':');
    if (colon > 0) {  // HZ:MS or R:MS
      String a = tok.substring(0, colon), b = tok.substring(colon + 1);
      int ms = b.toInt();
      bool rest = a == "R" || a == "r" || a == "0";
      int hz = rest ? 0 : a.toInt();
      if (ms <= 0 || (!rest && (hz < 20 || hz > 20000))) {
        err = "bad tone '" + tok + "': use HZ:MS with 20-20000 Hz";
        return -1;
      }
      t = {(uint16_t)hz, clampMs(ms), true, wave};
      out[n++] = t;
      continue;
    }
    int i = 0;
    int dur = 4, dots = 0;
    bool rest = false;
    int semi = 0, octave = 4;
    if (tok[0] == 'R' || tok[0] == 'r') {
      rest = true;
      i = 1;
    } else {
      semi = letterSemitone(tok[0]);
      if (semi < 0) {
        err = "bad note '" + tok + "': use C5/8, F#4/4., R/16 or 880:50";
        return -1;
      }
      i = 1;
      if (i < (int)tok.length() && tok[i] == '#') { semi++; i++; }
      else if (i < (int)tok.length() && tok[i] == 'b') { semi--; i++; }
      if (i < (int)tok.length() && isdigit((unsigned char)tok[i])) octave = tok[i++] - '0';
    }
    if (i < (int)tok.length() && tok[i] == '/') {
      i++;
      int d = 0;
      while (i < (int)tok.length() && isdigit((unsigned char)tok[i])) d = d * 10 + (tok[i++] - '0');
      if (!validDuration(d)) {
        err = "bad duration in '" + tok + "': use /1 /2 /4 /8 /16 /32 /64";
        return -1;
      }
      dur = d;
    }
    while (i < (int)tok.length() && tok[i] == '.') { dots++; i++; }
    if (i != (int)tok.length()) {
      err = "bad note '" + tok + "': use C5/8, F#4/4., R/16 or 880:50";
      return -1;
    }
    float ms = quarterMs * 4.0f / dur;
    if (dots == 1) ms *= 1.5f;
    else if (dots >= 2) ms *= 1.75f;
    t.ms = clampMs(ms);
    t.hz = rest ? 0 : noteHz(semi, octave);
    out[n++] = t;
  }
  if (n == 0) {
    err = "no notes";
    return -1;
  }
  return n;
}

// RTTTL: "name:d=4,o=5,b=100:8e6,8d6,p,4c#.6". Defaults: d=4, o=6, b=63.
int soundParseRtttl(const char *rtttl, Tone *out, int max, String &err) {
  String s(rtttl ? rtttl : "");
  int c1 = s.indexOf(':'), c2 = c1 < 0 ? -1 : s.indexOf(':', c1 + 1);
  if (c1 < 0 || c2 < 0) {
    err = "RTTTL needs name:settings:notes";
    return -1;
  }
  int defDur = 4, defOct = 6, bpm = 63;
  String settingsPart = s.substring(c1 + 1, c2);
  settingsPart.replace(" ", "");
  int pos = 0;
  while (pos < (int)settingsPart.length()) {
    int comma = settingsPart.indexOf(',', pos);
    if (comma < 0) comma = settingsPart.length();
    String kv = settingsPart.substring(pos, comma);
    pos = comma + 1;
    if (kv.length() < 3 || kv[1] != '=') continue;
    int v = kv.substring(2).toInt();
    char k = kv[0] | 0x20;
    if (k == 'd' && validDuration(v)) defDur = v;
    else if (k == 'o' && v >= 3 && v <= 8) defOct = v;
    else if (k == 'b' && v >= 20 && v <= 900) bpm = v;
  }
  float wholeMs = 60000.0f / bpm * 4.0f;
  String body = s.substring(c2 + 1);
  int n = 0;
  pos = 0;
  while (pos < (int)body.length()) {
    int comma = body.indexOf(',', pos);
    if (comma < 0) comma = body.length();
    String tok = body.substring(pos, comma);
    tok.trim();
    tok.toLowerCase();
    pos = comma + 1;
    if (tok.isEmpty()) continue;
    if (n >= max) {
      err = "too many notes (max " + String(max) + ")";
      return -1;
    }
    int i = 0, dur = 0;
    while (i < (int)tok.length() && isdigit((unsigned char)tok[i])) dur = dur * 10 + (tok[i++] - '0');
    if (!dur) dur = defDur;
    if (!validDuration(dur) || i >= (int)tok.length()) {
      err = "bad RTTTL note '" + tok + "'";
      return -1;
    }
    char letter = tok[i++];
    bool rest = letter == 'p';
    int semi = rest ? 0 : letterSemitone(letter);
    if (!rest && semi < 0) {
      err = "bad RTTTL note '" + tok + "'";
      return -1;
    }
    if (i < (int)tok.length() && tok[i] == '#') { semi++; i++; }
    bool dotted = false;
    if (i < (int)tok.length() && tok[i] == '.') { dotted = true; i++; }
    int oct = defOct;
    if (i < (int)tok.length() && isdigit((unsigned char)tok[i])) oct = tok[i++] - '0';
    if (i < (int)tok.length() && tok[i] == '.') { dotted = true; i++; }
    float ms = wholeMs / dur * (dotted ? 1.5f : 1.0f);
    out[n++] = {(uint16_t)(rest ? 0 : noteHz(semi, oct)), clampMs(ms), false, WAVE_DEFAULT};
  }
  if (n == 0) {
    err = "no notes";
    return -1;
  }
  return n;
}

// ------------------------------------------------------------------ playback

bool soundPlayNotes(const char *notes, int tempo, int repeat, String &err) {
  int n = soundParseNotes(notes, tempo, seq, SOUND_MAX_TONES, err);
  if (n < 0) return false;
  seqLen = n;
  startSequence(SND_MELODY, repeat);
  return true;
}

bool soundPlayRtttl(const char *rtttl, int repeat, String &err) {
  int n = soundParseRtttl(rtttl, seq, SOUND_MAX_TONES, err);
  if (n < 0) return false;
  seqLen = n;
  startSequence(SND_MELODY, repeat);
  return true;
}

static bool sfxAllowed() { return activePin >= 0 && cfg.effects && cfg.volume > 0 && !(soundPlaying() && prio == SND_MELODY); }

int soundSfxFind(const char *name) {
  for (int i = 0; i < SFX_COUNT; i++)
    if (name && !strcmp(name, SFX[i].name)) return i;
  return -1;
}

bool soundSfxExists(const char *name) { return soundSfxFind(name) >= 0; }

bool soundPlaySfx(SfxId id) {
  if (id >= SFX_COUNT) return false;
  if (!sfxAllowed()) return true;
  String err;
  int n = soundParseNotes(SFX[id].notes, SFX[id].tempo, seq, SOUND_MAX_TONES, err);
  if (n > 0) {
    seqLen = n;
    startSequence(SND_SFX, 1);
  }
  return true;
}

bool soundPlaySfx(SfxId id, int volume) {
  if (id >= SFX_COUNT) return false;
  if (!sfxAllowed()) return true;
  String err;
  int n = soundParseNotes(SFX[id].notes, SFX[id].tempo, seq, SOUND_MAX_TONES, err);
  if (n > 0) {
    seqLen = n;
    startSequence(SND_SFX, 1, volume < 0 ? -1 : constrain(volume, 0, 100));
  }
  return true;
}

bool soundPlaySfx(const char *name) {
  int i = soundSfxFind(name);
  return i >= 0 && soundPlaySfx((SfxId)i);
}

// Drops the first `offsetMs` of a parsed sequence (whole tones, then part of one). Returns the new length.
static int trimTones(Tone *t, int n, uint32_t offsetMs) {
  int skip = 0;
  while (skip < n && offsetMs >= t[skip].ms) offsetMs -= t[skip++].ms;
  n -= skip;
  if (skip && n > 0) memmove(t, t + skip, sizeof(Tone) * n);
  if (n > 0) t[0].ms -= offsetMs;
  return max(n, 0);
}

bool soundPlaySfxFrom(SfxId id, int volume, uint32_t offsetMs, bool dryRun) {
  if (id >= SFX_COUNT) return false;
  static Tone scratch[SOUND_MAX_TONES];  // a dry run must not touch seq (it may be playing)
  Tone *t = dryRun ? scratch : seq;
  if (!dryRun && !sfxAllowed()) return true;
  String err;
  int n = soundParseNotes(SFX[id].notes, SFX[id].tempo, t, SOUND_MAX_TONES, err);
  if (n <= 0) return true;
  n = trimTones(t, n, offsetMs);
  if (n <= 0) return true;
  uint32_t lead = 0;
  for (int i = 0; i < n && !t[i].hz; i++) lead += t[i].ms;
  const uint32_t dur = soundDurationMs(t, n);
  const uint32_t no = ++reqCounter;
#if SOUND_I2S
  if (dryRun) {
    if (activePin >= 0) synthDry(no, lead, dur);
    return true;
  }
  tagNo = no;
  tagLead = lead;
  tagDur = dur;
#else
  (void)lead;
  (void)dur;
  (void)no;
  if (dryRun) return true;
#endif
  seqLen = n;
  startSequence(SND_SFX, 1, volume < 0 ? -1 : constrain(volume, 0, 100));
  return true;
}

uint32_t soundLastRequest() { return reqCounter; }

bool soundLastStart(SoundStartInfo &out) {
#if SOUND_I2S
  portENTER_CRITICAL(&synthMux);
  out = lastStart;
  portEXIT_CRITICAL(&synthMux);
  return reqCounter && out.request == reqCounter;
#else
  (void)out;
  return false;
#endif
}

void soundBlip(uint16_t hz, uint16_t ms) {
  if (!sfxAllowed() || soundPlaying()) return;  // never cut another sound short
  seq[0] = {hz, ms, true, WAVE_DEFAULT};
  seqLen = 1;
  startSequence(SND_SFX, 1);
}

void soundTone(uint16_t hz, uint16_t ms) {
  soundToneEx(hz, ms, WAVE_DEFAULT, -1);
}

void soundToneEx(uint16_t hz, uint16_t ms, SoundWave wave, int volume) {
  seq[0] = {(uint16_t)constrain(hz, 20, 20000), (uint16_t)constrain(ms, 1, 10000), true, wave};
  seqLen = 1;
  startSequence(SND_MELODY, 1, volume < 0 ? -1 : constrain(volume, 0, 100));
}

const char *soundWaveName(SoundWave w) {
  if (w == WAVE_DEFAULT) w = (SoundWave)SOUND_DEFAULT_WAVE;
  return w == WAVE_SINE ? "sine" : w == WAVE_TRI ? "tri" : "square";
}

bool soundWaveFromName(const char *name, SoundWave &out) {
  if (!name) return false;
  if (!strcasecmp(name, "square")) out = WAVE_SQUARE;
  else if (!strcasecmp(name, "sine")) out = WAVE_SINE;
  else if (!strcasecmp(name, "tri") || !strcasecmp(name, "triangle")) out = WAVE_TRI;
  else return false;
  return true;
}

void soundDiag(Print &out) {
#if SOUND_I2S
  out.printf("sound: i2s=%d codec=%d task=%d playing=%d remain=%ums wave=%s (last %s) pa=%d blocks=%u short=%u\n",
             i2sOk, codecOk, synthTask != nullptr, (int)tPlaying, (unsigned)tRemainMs, soundWaveName((SoundWave)tWave),
             soundWaveName((SoundWave)tLastWave), (int)paOn, (unsigned)blocksWritten, (unsigned)shortWrites);
#else
  out.printf("sound: pwm pin=%d\n", activePin);
#endif
}

uint32_t soundSampleRate() {
#if SOUND_I2S
  return i2sOk ? SOUND_RATE : 0;
#else
  return 0;
#endif
}

size_t soundInputRead(int16_t *frames, size_t count, uint32_t timeoutMs) {
#if SOUND_I2S
  // Blocks until `count` frames arrived; the bus runs continuously, so the wait is the frames' own
  // duration. (I2SClass has one timeout for reads and writes: it is left at its default.)
  (void)timeoutMs;
  if (!i2sOk) return 0;
  return i2s.readBytes((char *)frames, count * 4) / 4;
#else
  (void)frames;
  (void)count;
  (void)timeoutMs;
  return 0;
#endif
}

const char *soundOutputName() {
#if SOUND_I2S
  if (!i2sTried) return "i2s";
  return codecOk ? "i2s" : "i2s-failed";
#else
  return PIN_COUNT ? "pwm" : "none";
#endif
}
