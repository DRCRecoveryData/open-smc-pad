#pragma once
#include <Arduino.h>

void midiInit();
void midiHandleOut(const uint8_t *packets, uint16_t len);
void midiTick();
