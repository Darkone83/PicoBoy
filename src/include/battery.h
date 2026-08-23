#pragma once
#include <stdint.h>
#include <stdbool.h>

// Battery monitor. VBAT is sampled through the 100k:100k divider on
// VBAT_SENSE_PIN (ADC3 / GPIO29) and filtered into an estimated single-cell
// Li-ion state of charge. PicoBoy PCB builds also use the TP4056 CHRG#/STDBY#
// inputs for deterministic charging/full state; builds without those pins fall
// back to the voltage/trend logic in battery.c.
//
// PCB-only: on variants without VBAT_SENSE_PIN these APIs are harmless stubs so
// the common UI/LED code still links.

void     battery_init(void);         // ADC + start the 1 s poll timer
uint32_t battery_millivolts(void);   // smoothed VBAT in mV; 0 == no valid reading
int      battery_percent(void);      // 0..100 state-of-charge from the curve
bool     battery_is_charging(void);  // TP4056 status when available; voltage/trend fallback otherwise
bool     battery_is_full(void);      // TP4056 STDBY#/voltage threshold with hysteresis