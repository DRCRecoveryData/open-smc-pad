#pragma once
#include <Arduino.h>

#include "proto.h"

void linkInit();
void linkSend(uint8_t type, const void *payload, uint16_t len);
void linkLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
