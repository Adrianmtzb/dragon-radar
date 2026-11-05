#pragma once
// The info card, as on the sibling apps: name, firmware and board, then WiFi (joined network and
// signal / setup network / connecting / off) with the board's addresses, the battery and the
// author's site. It takes over the panel (radarSuspend) and gives it back as it was.
#include <Arduino.h>

// full: the whole card; short: name, version and board only (after the boot animation).
void infoShow(bool full, uint32_t ms);
void infoHide();
bool infoShowing();
void infoLoop();  // closes it after its time, refreshes what changed (WiFi, battery)
