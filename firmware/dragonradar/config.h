// Project-wide constants. The board (pinout and capabilities) comes from board.h.
#pragma once
#include "board.h"  // always first
#include <Arduino.h>

#define FW_NAME "Dragon Radar"
#define FW_VERSION "0.1.0"
#define PROJECT_URL "https://github.com/Adrianmtzb/dragon-radar"  // the repo (setup page and panel credits)
#define AUTHOR_URL "https://adrianmb.dev"
#define AUTHOR_SHORT "adrianmb.dev"  // splash credit
#define AP_SSID_PREFIX "DragonRadar-"
#define DEFAULT_TZ "CST6"
#define MDNS_SERVICE "dragonradar"
#define DEFAULT_HOSTNAME "dragonradar"
#define NTP_SERVER "pool.ntp.org"
#define SETUP_ACCENT "#2bd66b"   // radar green; buttons use dark text on it (AA)
#define CLI_MAX_LINE 512
#define DONATE_URL "https://orbi.finance/app/orbi-pay?orbiId=A000001&amount=1.00&asset=USDT&concept=thanks-dragonradar"

// App state in Preferences (devsettings.h keeps "net" and "hw").
#define APP_PREFS_NS "dradar"

// Screen (devsettings.cpp): brightness 5-255 and the cap at night (22:00-07:00) while night
// dimming is on. On the AMOLED it is the panel's brightness command.
#define DEFAULT_BACKLIGHT 200
#define DEFAULT_NIGHT_DIM 0
#define BACKLIGHT_NIGHT 60
#define RENDER_FPS 30            // redraw cap of the main loop

// Battery monitor (boards with HAS_BATTERY). Percent thresholds carry hysteresis so the flag does
// not flap around the limit.
#define BATTERY_LOW_PCT 15
#define BATTERY_LOW_CLEAR_PCT 20
#define BATTERY_LOW_REMIND_MS (10UL * 60 * 1000)
#define BATTERY_CHARGE_RISE_MV 40  // ADC boards only: voltage climb over the window that reads as "charging"
#define BATTERY_TREND_WINDOW_MS 60000
#define BATTERY_TREND_STEP_MS 5000
#define POWER_OFF_HOLD_MS 2000     // boards with a power latch

// Keys and touch gestures (hw.cpp, touch.cpp).
#define BUTTON_DOUBLE_CLICK_MS 350
#define BUTTON_LONG_PRESS_MS 600
#define BUTTON_RESET_PRESS_MS 6000

// Sound (sound.cpp). Volume 0-100 on the curve of sound.h; the speaker is loud, so 40 by default.
#define SOUND_DEFAULT_VOLUME 40
#define SOUND_DEFAULT_WAVE WAVE_SQUARE
#define SOUND_MAX_TONES 512
#define LED_APP_MODE_NAME "app"
#define LED_DEFAULT_BRIGHTNESS 40
