#pragma once
#include <stdint.h>
#include <stdbool.h>

// Battery monitor -- VOLTAGE ONLY. Reads VBAT through the 100k:100k divider on
// VBAT_SENSE_PIN (ADC3 / GPIO29) and estimates state-of-charge from a single-cell
// Li-ion discharge curve. There are no TP4056 CHRG#/STDBY# status pins wired, so
// "full" is inferred from voltage and "charging" is an optional rising-voltage
// heuristic (off by default -- see BATTERY_DETECT_CHARGING in battery.c).
//
// PCB-only: on the breadboard variant VBAT_SENSE_PIN is undefined and these are
// no-ops so main.c still links.
//
// Calibration + a serial debug print live at the top of battery.c. If the meter
// reads wrong, set BATTERY_DEBUG 1, open the USB serial monitor, and read the
// raw/mV numbers -- that tells you immediately whether it's the ADC, the divider,
// or the curve.

void     battery_init(void);         // ADC + start the 1 s poll timer
uint32_t battery_millivolts(void);   // smoothed VBAT in mV; 0 == no valid reading
int      battery_percent(void);      // 0..100 state-of-charge from the curve
bool     battery_is_charging(void);  // best-effort (heuristic); false if not detected
bool     battery_is_full(void);      // VBAT at/above the full threshold