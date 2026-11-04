#pragma once
// Board selection, shared by every app built on this base: one header per supported board under
// boards/, picked by the compiler target so the same sketch builds for every board with nothing
// more than the right FQBN (see the Makefile). Boards that share a target are told apart with a
// define the Makefile passes, checked first. The app's config.h includes this before anything else.
#include <sdkconfig.h>  // CONFIG_IDF_TARGET_* comes from the core

#if defined(BOARD_S3_AMOLED175)
#include "boards/s3_amoled175.h"
#elif defined(BOARD_S3_LCD128)
#include "boards/s3_lcd128.h"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#include "boards/c6_lcd147.h"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#include "boards/s3_touch169.h"
#else
#error "Unsupported target: add a header under boards/ and select it in board.h"
#endif

// Offsets for the mirrored rotations default to the plain ones unless the board says otherwise.
#ifndef LCD_COL_OFFSET2
#define LCD_COL_OFFSET2 LCD_COL_OFFSET
#endif
#ifndef LCD_ROW_OFFSET2
#define LCD_ROW_OFFSET2 LCD_ROW_OFFSET
#endif
