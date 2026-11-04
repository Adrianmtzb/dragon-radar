// Buzzer: non-blocking melody player, built-in sound effects (sounds.h, generated from
// shared/sounds.json by tools/gen_sounds.mjs) and melody parsers. The buzzer is the board's own
// (HAS_BUZZER) or a passive one wired to one of the GPIOs the board header allows (BUZZER_PINS);
// an external one starts disabled. Every call is safe on a board with no buzzer pin at all.
// On boards with an I2S codec (AUDIO_ES8311) the "buzzer" is the speaker: a synth task renders
// square or sine tones into the codec, and the amplifier (PIN_PA_CTRL) is only on while sounding.
//
// Melody format: whitespace-separated tokens
//   NOTE[#|b]OCTAVE[/DURATION][.]   C5/8  F#4/4.  Bb3/2  (duration 1,2,4,8,16,32,64; default 4)
//   R[/DURATION][.]                 rest
//   HZ:MS or R:MS                   raw tone or rest in milliseconds (1800:20)
//   ~square | ~sine                 waveform of the tones that follow (codec boards; a piezo is
//                                   always square). Default SOUND_DEFAULT_WAVE.
// Tempo is in quarter notes per minute. RTTTL ("name:d=4,o=5,b=100:8e6,8d6") is also accepted.
//
// Volume 0-100 follows one curve on every app: duty = 50 % x (volume / 100)^2. A piezo is loudest
// at 50 % duty and its level drops roughly with the square of the duty below that, so the curve
// feels even; 100 is the plain 50 % square wave. Optional config.h macros: SOUND_DEFAULT_VOLUME
// (default 100), SOUND_MAX_TONES (longest melody; default 512, 256 on boards without PSRAM).
#pragma once
#include "config.h"
#include <Arduino.h>
#if __has_include("sounds.h")
#include "sounds.h"
#else
// No shared/sounds.json yet: an empty catalogue, so the player still builds.
#include <stdint.h>
struct SfxDef {
  const char *name;
  uint16_t tempo;
  const char *notes;
};
enum SfxId : uint8_t { SFX_COUNT };
static const SfxDef *const SFX = nullptr;
#endif

#ifndef SOUND_MAX_TONES
#define SOUND_MAX_TONES (BOARD_PSRAM_MB > 0 ? 512 : 256)
#endif
#ifndef SOUND_DEFAULT_VOLUME
#define SOUND_DEFAULT_VOLUME 100
#endif

enum SoundWave : uint8_t { WAVE_DEFAULT = 0, WAVE_SQUARE = 1, WAVE_SINE = 2 };
#ifndef SOUND_DEFAULT_WAVE
#define SOUND_DEFAULT_WAVE WAVE_SQUARE
#endif

struct Tone {
  uint16_t hz;  // 0 = rest
  uint16_t ms;
  bool legato;  // raw HZ:MS tones run into each other; notes get a short articulation gap
  uint8_t wave; // SoundWave; WAVE_DEFAULT plays SOUND_DEFAULT_WAVE
};

enum SoundPriority : uint8_t { SND_SFX = 1, SND_MELODY = 2 };

struct SoundConfig {
  bool enabled;    // drive the buzzer at all
  int8_t pin;      // -1 when the board allows no buzzer pin
  uint8_t volume;  // 0-100
  bool effects;    // play built-in effects (soundPlaySfx, soundBlip)
};

void soundBegin();
void soundLoop();

SoundConfig soundDefaults();
const SoundConfig &soundConfig();
// Validates (volume, allowed pin, no clash with a lit status LED) and applies. On failure nothing
// changes and `err` says why. Callable before soundBegin(), e.g. when loading saved settings.
bool soundConfigure(const SoundConfig &cfg, String &err);

bool soundAvailable();     // the board has a buzzer or at least one allowed buzzer pin
bool soundBuiltin();       // HAS_BUZZER
bool soundHasBuzzer();     // a buzzer is enabled and attached
bool soundPinAllowed(int pin);
int soundPinCount();
int soundPinAt(int i);
bool soundUsesPin(int pin);  // the buzzer is enabled on this pin

// Parse without playing. Returns the number of tones written to `out` (up to `max`), or -1 with `err`.
int soundParseNotes(const char *notes, int tempo, Tone *out, int max, String &err);
int soundParseRtttl(const char *rtttl, Tone *out, int max, String &err);

// Melodies: replace whatever is playing. `repeat` 1-20.
bool soundPlayNotes(const char *notes, int tempo, int repeat, String &err);
bool soundPlayRtttl(const char *rtttl, int repeat, String &err);
// Built-in effect; plays only when effects are on and no melody is playing, and replaces another
// effect. Returns false for an unknown name.
bool soundPlaySfx(SfxId id);
bool soundPlaySfx(const char *name);
int soundSfxFind(const char *name);  // index into SFX, or -1
bool soundSfxExists(const char *name);
// Short blip for effects; same gating as soundPlaySfx and never cuts another sound short.
void soundBlip(uint16_t hz, uint16_t ms);
void soundTone(uint16_t hz, uint16_t ms);  // test tone, melody priority
// Test tone with a waveform and a one-off volume 0-100 (-1 = the configured one), melody priority.
void soundToneEx(uint16_t hz, uint16_t ms, SoundWave wave, int volume);
const char *soundWaveName(SoundWave wave);  // "square", "sine"
bool soundWaveFromName(const char *name, SoundWave &out);
// Output path: "i2s" (codec ready), "i2s-failed", "pwm" (piezo on LEDC) or "none".
const char *soundOutputName();
void soundDiag(Print &out);  // one line of output-path internals, for bring-up
void soundStop();

bool soundPlaying();
uint32_t soundRemainingMs();
uint32_t soundDurationMs(const Tone *tones, int n);
