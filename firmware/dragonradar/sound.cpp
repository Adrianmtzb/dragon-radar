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

#define SOUND_RATE 24000      // MCLK 256 x fs = 6.144 MHz, a clock the ES8311 tables cover
#define SOUND_PA_OFF_MS 150
#define SOUND_PA_SETTLE_MS 8
#define SOUND_BLOCK 240       // frames per write: 10 ms
static I2SClass i2s;
static TaskHandle_t synthTask = nullptr;
static volatile uint32_t synthStep = 0;     // phase increment per sample (32-bit phase)
static volatile int32_t synthTarget = 0;    // target amplitude 0-32767
static volatile uint8_t synthWave = WAVE_SQUARE;
static bool i2sOk = false, codecOk = false, i2sTried = false;
static volatile bool paOn = false;
static volatile uint32_t blocksWritten = 0, shortWrites = 0;
static int16_t sineTable[256];

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
// for slave mode, MCLK from its pin at 256 x fs, 16-bit I2S, fs = SOUND_RATE.
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

static void synthLoop(void *) {
  static int16_t buf[SOUND_BLOCK * 2];
  uint32_t phase = 0;
  int32_t amp = 0;
  bool pa = false;
  uint32_t silentSince = 0, settleUntil = 0;
  const int32_t RAMP = 32767 / (SOUND_RATE / 500);  // full scale in ~2 ms
  for (;;) {
    const int32_t want = synthTarget;
    uint32_t now = millis();
    if (want > 0) {
      silentSince = 0;
      if (!pa) {
        paSet(pa = true);
        settleUntil = now + SOUND_PA_SETTLE_MS;  // let the amplifier wake before the tone starts
      }
    }
    // Silent while the amplifier settles; that wait does not count as silence for switching it off.
    int32_t target = (int32_t)(now - settleUntil) < 0 ? 0 : want;
    if (want == 0 && amp == 0) {
      if (!silentSince) silentSince = now | 1;
      if (pa && (int32_t)(now - silentSince) >= SOUND_PA_OFF_MS) paSet(pa = false);
      if (!pa) {  // idle: sleep until output() has something to play
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        continue;
      }
    }
    uint32_t step = synthStep;
    uint8_t wave = synthWave;
    for (int i = 0; i < SOUND_BLOCK; i++) {
      if (amp < target) amp = min(target, amp + RAMP);
      else if (amp > target) amp = max(target, amp - RAMP);
      phase += step;
      int32_t s = wave == WAVE_SINE ? sineTable[phase >> 24] : ((phase & 0x80000000u) ? -32767 : 32767);
      int16_t v = (int16_t)(s * amp / 32767);
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
  for (int i = 0; i < 256; i++) sineTable[i] = (int16_t)lroundf(32767.0f * sinf(i * 2.0f * (float)M_PI / 256.0f));
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

static void synthSet(uint16_t hz, uint8_t wave, int volume) {
  if (!hz || volume <= 0) {
    synthTarget = 0;
    return;
  }
  hz = min<uint16_t>(hz, SOUND_RATE / 2 - 1);
  synthStep = (uint32_t)((uint64_t)hz * 4294967296ULL / SOUND_RATE);
  synthWave = wave == WAVE_DEFAULT ? SOUND_DEFAULT_WAVE : wave;
  // Same perceptual curve as the piezo: level grows with the square of the volume.
  synthTarget = max<int32_t>(1, 30000L * volume * volume / 10000);
  if (synthTask) xTaskNotifyGive(synthTask);
}
#endif

// ------------------------------------------------------------------ output

static void output(uint16_t hz) {
  if (activePin < 0) return;
#if SOUND_I2S
  synthSet(hz, curWave, seqVolume >= 0 ? seqVolume : cfg.volume);
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
  if (activePin >= 0) synthSet(0, 0, 0);
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
  if (playing) startTone(millis());
  else output(0);
}

void soundLoop() {
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
  output(0);
}

bool soundPlaying() { return playing; }
uint32_t soundRemainingMs() { return playing ? remainingMs : 0; }

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
        err = "bad waveform '" + tok + "': use ~square or ~sine";
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

static bool sfxAllowed() { return activePin >= 0 && cfg.effects && cfg.volume > 0 && !(playing && prio == SND_MELODY); }

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

bool soundPlaySfx(const char *name) {
  int i = soundSfxFind(name);
  return i >= 0 && soundPlaySfx((SfxId)i);
}

void soundBlip(uint16_t hz, uint16_t ms) {
  if (!sfxAllowed() || playing) return;  // never cut another sound short
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
  return w == WAVE_SINE ? "sine" : "square";
}

bool soundWaveFromName(const char *name, SoundWave &out) {
  if (!name) return false;
  if (!strcasecmp(name, "square")) out = WAVE_SQUARE;
  else if (!strcasecmp(name, "sine")) out = WAVE_SINE;
  else return false;
  return true;
}

void soundDiag(Print &out) {
#if SOUND_I2S
  out.printf("sound: i2s=%d codec=%d task=%d target=%d pa=%d blocks=%u short=%u\n", i2sOk, codecOk, synthTask != nullptr,
             (int)synthTarget, (int)paOn, (unsigned)blocksWritten, (unsigned)shortWrites);
#else
  out.printf("sound: pwm pin=%d\n", activePin);
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
