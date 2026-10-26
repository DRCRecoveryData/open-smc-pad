/* Busy-wait loops counted in iterations were tuned with the CPU at 24 MHz. SMCPAD_SPIN(n) is
 * n scaled to the configured clock (TCFG_CLOCK_SYS_HZ), so each wait keeps its length. */
#pragma once
#include "app_config.h"

#define SMCPAD_SPIN_SCALE   ((TCFG_CLOCK_SYS_HZ + 23999999) / 24000000)
#define SMCPAD_SPIN(n)      ((n) * SMCPAD_SPIN_SCALE)
