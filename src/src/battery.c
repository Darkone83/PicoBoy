#include "battery.h"
#include "pins.h"
#include "led.h"

#ifdef VBAT_SENSE_PIN
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include <stdio.h>

// ===========================================================================
//  CALIBRATION  --  enable BATTERY_DEBUG to see raw/mV/%/state over USB serial.
// ===========================================================================
#define BATTERY_DEBUG          0      // 1 = printf each poll (USB CDC)

#define VBAT_ADC_CH            3      // ADC3 == GPIO29 (VBAT_SENSE_PIN)
#define VREF_MV               3300u   // ADC reference; measure your 3V3 and set exactly
#define ADC_FULL_SCALE        4095u
#define VBAT_R_TOP            100u     // R13 (VBAT -> node), kohm
#define VBAT_R_BOT            100u     // R14 (node -> GND), kohm
#define VBAT_CAL_NUM         1000u     // fine trim: reading * NUM/DEN (start 1000/1000)
#define VBAT_CAL_DEN         1000u
#define VBAT_VALID_MIN_MV    2500u     // below this = no cell / sense line dead

// ---- State thresholds -------------------------------------------------------
#define FULL_PCT               95      // percentage fallback for non-status-pin builds
#define FULL_ENTER_MV        4120u     // charger present + near-full voltage -> green
#define FULL_EXIT_MV         4050u     // full hysteresis while charger remains present
#define LOW_ENTER_PCT          20      // amber warning at/under 20%
#define LOW_EXIT_PCT           25      // clear amber above 25%
#define CRIT_ENTER_PCT         10      // fast red pulse at/under 10%
#define CRIT_EXIT_PCT          13      // clear critical above 13% (then LOW remains)

// Charging (voltage-only heuristic). CHARGE DETECTION FROM VOLTAGE IS APPROXIMATE:
// a charging cell rises only a few mV/min, near the ADC noise floor. The window is
// long and the decision debounced to keep it stable. If TP4056 CHRG#/STDBY# are
// wired, those pins are used first, with voltage retained as a near-full fallback.
#define TREND_WINDOW           60      // seconds (samples at POLL_MS) for the slope
#define CHG_RISE_MV            10      // EMA rise over the window that counts as charging
#define DEBOUNCE               3       // consecutive polls before charging flips

#define POLL_MS              1000
#define ADC_SAMPLES            16      // per poll; trimmed-mean (drop min & max)

// Single-cell Li-ion resting OCV (mV) -> state-of-charge (%). Flat middle, steep ends.
static const uint16_t k_ocv[][2] = {
    {4200,100},{4100,94},{4000,85},{3950,76},{3900,68},{3850,58},
    {3800,50},{3780,42},{3750,34},{3700,25},{3650,18},{3600,12},
    {3500,6},{3400,2},{3300,0},
};

static uint32_t s_mv=0, s_ema=0;
static bool     s_ema_init=false;
static int      s_pct=0;
static bool     s_full=false, s_charging=false, s_low=false, s_critical=false;
static int      s_chg_cnt=0;              // debounce accumulator
static uint16_t s_hist[TREND_WINDOW];     // ring of EMA samples
static int      s_hist_i=0;
static repeating_timer_t s_timer;

static bool valid(void) { return s_mv >= VBAT_VALID_MIN_MV; }

static int pct_from_mv(uint32_t mv) {
    const int n = (int)(sizeof(k_ocv)/sizeof(k_ocv[0]));
    if (mv >= k_ocv[0][0])   return 100;
    if (mv <= k_ocv[n-1][0]) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= k_ocv[i][0]) {
            uint32_t hi=k_ocv[i-1][0], lo=k_ocv[i][0];
            int      ph=k_ocv[i-1][1], pl=k_ocv[i][1];
            return pl + (int)((mv-lo) * (uint32_t)(ph-pl) / (hi-lo));
        }
    }
    return 0;
}

static uint32_t read_vbat_mv(void) {
    adc_select_input(VBAT_ADC_CH);
    (void)adc_read();                                 // discard first after mux switch
    uint32_t mn=0xFFFF, mx=0, sum=0;
    for (int i=0;i<ADC_SAMPLES;i++){ uint32_t v=adc_read(); sum+=v; if(v<mn)mn=v; if(v>mx)mx=v; }
    uint32_t raw = (sum-mn-mx)/(ADC_SAMPLES-2);
    uint32_t adc_mv = raw*VREF_MV/ADC_FULL_SCALE;
    uint32_t vbat = adc_mv*(VBAT_R_TOP+VBAT_R_BOT)/VBAT_R_BOT;
    return vbat*VBAT_CAL_NUM/VBAT_CAL_DEN;
}

static bool poll(repeating_timer_t *t) {
    (void)t;
    s_mv = read_vbat_mv();

    if (!valid()) {
        s_ema_init=false; s_pct=0; s_full=s_charging=s_low=s_critical=false; s_chg_cnt=0;
        led_set_battery(LED_BATT_NONE);
#if BATTERY_DEBUG
        printf("[batt] vbat=%lumV -> no valid reading (check sense line / ADC_AVDD)\n",
               (unsigned long)s_mv);
#endif
        return true;
    }

    if (!s_ema_init) {                                 // seed EMA + prefill ring (no jump)
        s_ema = s_mv; s_ema_init = true;
        for (int i=0;i<TREND_WINDOW;i++) s_hist[i]=(uint16_t)s_ema;
        s_hist_i = 0;
    } else {
        s_ema = (s_ema*3 + s_mv)/4;                    // EMA, alpha 1/4
    }

    s_pct = pct_from_mv(s_ema);

    uint16_t oldest = s_hist[s_hist_i];                // value ~TREND_WINDOW s ago
    int slope = (int)s_ema - (int)oldest;
    s_hist[s_hist_i] = (uint16_t)s_ema;
    s_hist_i = (s_hist_i+1) % TREND_WINDOW;

    bool charging = false;
    bool full_hw = false;
    bool charger_present = false;

#if defined(CHRG_PIN)
    (void)slope;
    charging = (gpio_get(CHRG_PIN) == 0);              // TP4056 CHRG#: low = charging
  #if defined(STDBY_PIN)
    full_hw = (gpio_get(STDBY_PIN) == 0);              // TP4056 STDBY#: low = complete
  #endif
    // With both TP4056 status outputs high the charger is effectively absent.
    // This prevents a freshly-unplugged, high-voltage battery from staying green.
    charger_present = charging || full_hw;
#else
    // Voltage-only fallback for boards without TP4056 status GPIOs.
    bool rising = (slope > CHG_RISE_MV);
    s_chg_cnt = rising ? (s_chg_cnt < DEBOUNCE ? s_chg_cnt+1 : DEBOUNCE)
                       : (s_chg_cnt > 0 ? s_chg_cnt-1 : 0);
    charging = (s_chg_cnt >= DEBOUNCE);
    charger_present = charging;
#endif

#if defined(CHRG_PIN) && defined(STDBY_PIN)
    // Green requires BOTH evidence of external charge power and a near-full
    // condition. STDBY# is authoritative; voltage is the useful fallback while
    // CHRG# is still tapering near the top of charge.
    if (!charger_present) {
        s_full = false;                                // unplugged -> never stay green
    } else if (full_hw || s_ema >= FULL_ENTER_MV) {
        s_full = true;
    } else if (s_full && s_ema < FULL_EXIT_MV) {
        s_full = false;
    }
#else
    // Generic fallback when deterministic charger status is unavailable.
    if (!charging && s_pct >= FULL_PCT) s_full = true;
    else if (s_pct < FULL_PCT)          s_full = false;
#endif

    // Near-full/full wins over orange charging while external power is present.
    s_charging = charging && !s_full;

    // Independent warning hysteresis. At <=10% CRITICAL supersedes LOW; when
    // CRITICAL clears above 13%, the normal <=20% LOW warning remains active.
    if (s_critical) { if (s_pct >= CRIT_EXIT_PCT)  s_critical=false; }
    else            { if (s_pct <= CRIT_ENTER_PCT) s_critical=true;  }

    if (s_low) { if (s_pct >= LOW_EXIT_PCT)  s_low=false; }
    else       { if (s_pct <= LOW_ENTER_PCT) s_low=true;  }

    led_batt_t b = s_full      ? LED_BATT_FULL
                 : s_charging  ? LED_BATT_CHARGING
                 : s_critical  ? LED_BATT_CRITICAL
                 : s_low       ? LED_BATT_LOW
                 :               LED_BATT_NONE;
    led_set_battery(b);

#if BATTERY_DEBUG
    printf("[batt] vbat=%lumV ema=%lumV pct=%d slope=%+dmV/%ds %s\n",
           (unsigned long)s_mv,(unsigned long)s_ema,s_pct,slope,TREND_WINDOW,
           s_full?"FULL":s_charging?"CHARGING":s_critical?"CRITICAL":s_low?"LOW":"-");
#endif
    return true;
}

void battery_init(void) {
    adc_init();
    adc_gpio_init(VBAT_SENSE_PIN);
#if defined(CHRG_PIN)
    gpio_init(CHRG_PIN);
    gpio_set_dir(CHRG_PIN, GPIO_IN);
    gpio_pull_up(CHRG_PIN);       // TP4056 status outputs are open-drain; internal pull-up is a safe fallback
#endif
#if defined(STDBY_PIN)
    gpio_init(STDBY_PIN);
    gpio_set_dir(STDBY_PIN, GPIO_IN);
    gpio_pull_up(STDBY_PIN);      // harmless in parallel with any external pull-up
#endif
    poll(NULL);
    add_repeating_timer_ms(POLL_MS, poll, NULL, &s_timer);
}

uint32_t battery_millivolts(void) { return valid() ? s_ema : 0; }
int      battery_percent(void)    { return valid() ? s_pct : 0; }
bool     battery_is_charging(void){ return s_charging; }
bool     battery_is_full(void)    { return s_full; }

#else  /* breadboard: no battery hardware -- stubs so main.c always links */
void     battery_init(void)        {}
uint32_t battery_millivolts(void)  { return 0; }
int      battery_percent(void)     { return 0; }
bool     battery_is_charging(void) { return false; }
bool     battery_is_full(void)     { return false; }
#endif