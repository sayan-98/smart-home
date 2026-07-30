// ---------------------------------------------------------------------------
//  Smart Home OS - ESP32 firmware entry point.
//
//  Everything lives in App. This file stays this short on purpose: the boot
//  order is a design decision documented in App.cpp, not something to be
//  accidentally reshuffled by editing a sketch.
//
//  Board:  ESP32 DevKit V1 (30-pin, WROOM-32, 4 MB)
//  Build:  pio run -e esp32dev
//  Flash:  pio run -e esp32dev -t upload -t monitor
// ---------------------------------------------------------------------------
#include <Arduino.h>

#include "core/App.h"

void setup() { sh::App::boot(); }

void loop() { sh::App::tick(); }
