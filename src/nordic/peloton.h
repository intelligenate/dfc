/*********************************************************************
 * peloton.h — Peloton bike serial protocol decoder + ride state
 *
 * Platform independent: no SDK dependencies. The caller supplies a
 * monotonic tick counter (`now`) with every byte; the module never
 * reads a clock itself. Designed to be dropped into the nRF5 SDK
 * firmware, an ESP32 build, or a host unit test unchanged.
 *
 * Protocol (decoded by Imran Haque / PeloMon,
 * https://ihaque.org/posts/2020/10/15/pelomon-part-i-decoding-peloton/):
 *
 *   Head unit -> bike:  F5 <id> <cs> F6            (4 bytes)
 *   Bike -> head unit:  F1 <id> <len> <digits> <cs> F6
 *     digits = <len> ASCII digits, LEAST significant first
 *     cs     = sum of every preceding byte, modulo 256
 *
 *   id 0x41  cadence     3 digits  rpm
 *   id 0x44  power       5 digits  deciwatts
 *   id 0x4A  resistance  4 digits  raw sensor value
 *   id 0x49  resistance level, 3 digits, 0..100 % (sent by current Peloton software; not in PeloMon)
 *   id 0xF7  resistance-table entry, 4 digits (31 of them at HU boot)
 *   id 0xFB  bike ID (7 bytes), id 0xFE init reply — HU boot only
 *
 * The head unit polls cadence/power/resistance round-robin every
 * ~100 ms during a ride, so each value refreshes about every 300 ms.
 *
 * Licensed under the Apache License, Version 2.0.
 * The default resistance table was captured from the owner's bike and the
 * power->speed curve is fitted from the owner's own rides
 * (scripts/fit_speed.py); nothing here is reproduced from PeloMon's code.
 *********************************************************************/
#ifndef PELOTON_H
#define PELOTON_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PELOTON_HEADER          0xF1
#define PELOTON_FOOTER          0xF6
#define PELOTON_ID_CADENCE      0x41
#define PELOTON_ID_POWER        0x44
#define PELOTON_ID_RESISTANCE   0x4A
#define PELOTON_ID_RES_LEVEL    0x49    /* 0..100 % as computed by the bike (newer firmware) */
#define PELOTON_ID_RLUT         0xF7
#define PELOTON_ID_BIKE_ID      0xFB
#define PELOTON_ID_INIT         0xFE

#define PELOTON_RLUT_ENTRIES    31
#define PELOTON_MAX_PAYLOAD     8

/* Tick counter width. The nRF app_timer uses the 24-bit RTC counter. */
#ifndef PELOTON_TICK_MASK
#define PELOTON_TICK_MASK       0x00FFFFFFu
#endif

/* Values are considered stale (and reported as 0) after this long. */
#ifndef PELOTON_DATA_TIMEOUT_MS
#define PELOTON_DATA_TIMEOUT_MS 5000
#endif

/* Wheel circumference used to turn speed into wheel revolutions for
 * CPS/CSC wheel data (700x25c = 2.105 m). Set the same wheel size on the
 * client (Apple Watch sensor settings) for matching distance. */
#ifndef PELOTON_WHEEL_CIRCUMFERENCE_M
#define PELOTON_WHEEL_CIRCUMFERENCE_M 2.105f
#endif

/* Max gap between resistance-table entries of one boot burst. */
#ifndef PELOTON_RLUT_GAP_MS
#define PELOTON_RLUT_GAP_MS     1000
#endif

typedef struct
{
    uint16_t cadence_rpm;
    uint16_t power_dw;          /* deciwatts, as transmitted by the bike */
    uint16_t resistance_raw;
    uint8_t  resistance_pct;    /* 0..100: from the bike's 0x49 message when it sends one, else via table */
    bool     res_level_from_bike;
    bool     cadence_valid;
    bool     power_valid;
    bool     resistance_valid;
    uint32_t cadence_ts;        /* tick of the last message of each kind */
    uint32_t power_ts;
    uint32_t resistance_ts;

    /* Synthesized crank events (for CPS / CSC crank revolution data) */
    float    crank_revs;        /* cumulative, fractional */
    uint32_t last_crank_ts;     /* tick at which the last whole rev completed */

    /* Synthesized wheel events from the power-derived speed (CPS wheel data) */
    float    wheel_revs;
    uint32_t last_wheel_ts;

    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t bad_cs;        /* checksum mismatch */
    uint32_t bad_footer;    /* checksum ok, footer not 0xF6 */
    uint32_t bad_len;       /* len byte 0 or > 8 */
    uint32_t bad_id;        /* unknown id, or wrong digit count for a known id */
    uint32_t bad_digit;     /* non-ASCII-digit payload */
} peloton_ride_t;

/** Reset all state. ticks_per_sec: rate of the `now` values you pass in. */
void     peloton_init(uint32_t ticks_per_sec);

/** Feed one received byte. `now` = current tick (only used on frame end). */
void     peloton_parse_byte(uint8_t byte, uint32_t now);

/** Drop any partial frame (call after a UART error). */
void     peloton_resync(void);

/** Invalidate values whose last update is older than the timeout. */
void     peloton_expire_stale(uint32_t now);

const peloton_ride_t * peloton_ride(void);

/** Power in whole watts (0 if invalid). */
uint16_t peloton_watts(void);

/** Cadence in rpm (0 if invalid). */
uint16_t peloton_cadence(void);

/** Resistance 0..100 (0 if invalid). */
uint8_t  peloton_resistance_pct(void);

/** CPS/CSC crank data: cumulative revolutions, and last event time in 1/1024 s. */
uint16_t peloton_crank_revs(void);
uint16_t peloton_crank_event_time(void);

/** CPS wheel data: cumulative revolutions, and last event time in 1/2048 s
 *  (CSC uses 1/1024 s — use peloton_wheel_event_time_1024 for that). */
uint32_t peloton_wheel_revs(void);
uint16_t peloton_wheel_event_time(void);
uint16_t peloton_wheel_event_time_1024(void);

/** Speed in km/h * 100 (FTMS units).
 *  from_power = true: PeloMon's power->speed regression (what the head unit shows).
 *  from_power = false: cadence * wheel_circumference_m * gear_ratio. */
/** Runtime calibration (phase 5 settings). Applied to the bike's deciwatts
 *  before anything else sees them: cal = raw * scale / 1000 + offset,
 *  rounded, clamped to 0..65535. Defaults 1000 / 0. Not reset by peloton_init. */
void     peloton_set_power_calibration(uint16_t scale_x1000, int16_t offset_dw);
/** Wheel circumference for wheel-revolution synthesis (default
 *  PELOTON_WHEEL_CIRCUMFERENCE_M). Not reset by peloton_init. */
void     peloton_set_wheel_circumference_mm(uint16_t mm);
uint16_t peloton_power_scale_x1000(void);
int16_t  peloton_power_offset_dw(void);
uint16_t peloton_wheel_circumference_mm(void);

uint16_t peloton_speed_kmh_x100(bool from_power, float wheel_circumference_m, float gear_ratio);
float    peloton_speed_mph_from_power(uint16_t deciwatts);

/** Resistance table access. */
bool             peloton_rlut_captured(void);   /* true once read from this bike */
const uint16_t * peloton_rlut(void);            /* 31 raw values, ascending */
uint8_t          peloton_rlut_translate(uint16_t raw);

#ifdef __cplusplus
}
#endif
#endif /* PELOTON_H */
