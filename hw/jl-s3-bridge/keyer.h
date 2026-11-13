#pragma once
#include "proto.h"

void keyerInit();
void keyerHandleCommand(const KeyerCmd &cmd);
void keyerTick();
