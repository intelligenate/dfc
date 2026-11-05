/*********************************************************************
 * peloton.c — Peloton bike serial protocol decoder + ride state
 * See peloton.h for the protocol description and API.
 *
 * Licensed under the Apache License, Version 2.0.
 *********************************************************************/
#include "peloton.h"
#include <string.h>
#include <math.h>

/*-------------------------------------------------------------------
 * State
 *-----------------------------------------------------------------*/

static peloton_ride_t m_ride;
static uint32_t       m_ticks_per_sec = 1000;

/* Runtime settings (phase 5): survive peloton_init() on purpose. */
static uint16_t m_cal_scale  = 1000;
static int16_t  m_cal_offset = 0;
static float    m_wheel_m    = PELOTON_WHEEL_CIRCUMFERENCE_M;

typedef enum
{
    ST_WAIT_HEADER,
    ST_ID,
    ST_LEN,
    ST_DATA,
    ST_CHECKSUM,
    ST_FOOTER
} state_t;

static state_t  m_state = ST_WAIT_HEADER;
static uint8_t  m_id;
static uint8_t  m_len;
static uint8_t  m_data[PELOTON_MAX_PAYLOAD];
static uint8_t  m_idx;
static uint8_t  m_cs;
static bool     m_cs_ok;

/* Resistance table */
static uint16_t m_rlut[PELOTON_RLUT_ENTRIES];
static bool     m_rlut_captured;
static uint16_t m_rlut_pending[PELOTON_RLUT_ENTRIES];
static uint8_t  m_rlut_pending_n;
static uint32_t m_rlut_pending_ts;

/* Default table: the 31 calibration points read from the owner's own bike
 * (2026 Peloton Bike, captured over the bus at head-unit boot on 2026-10-15
 * with the T command: 163 ... 953). Replaced at runtime once a bike's own
 * table is seen at head-unit boot. Stored as the first value and 30 deltas. */
static const uint8_t RLUT_DEFAULT_DELTA[PELOTON_RLUT_ENTRIES] =
{
    163,   6,  17,  40,  63,  61,  62,  66,
     56,  46,  42,  41,  35,  30,  28,  24,
     23,  19,  17,  16,  15,  11,  11,  12,
      9,   8,   9,   6,   7,   5,   5
};

/*-------------------------------------------------------------------
 * Helpers
 *-----------------------------------------------------------------*/

static inline uint32_t ms_to_ticks(uint32_t ms)
{
    return (uint32_t)(((uint64_t)ms * m_ticks_per_sec) / 1000u);
}

static inline uint32_t ticks_since(uint32_t then, uint32_t now)
{
    return (now - then) & PELOTON_TICK_MASK;
}

/*-------------------------------------------------------------------
 * Resistance table
 *-----------------------------------------------------------------*/

static void rlut_load_default(void)
{
    uint16_t acc = 0;
    for (uint8_t i = 0; i < PELOTON_RLUT_ENTRIES; i++)
    {
        acc += RLUT_DEFAULT_DELTA[i];
        m_rlut[i] = acc;
    }
    m_rlut_captured  = false;
    m_rlut_pending_n = 0;
}

uint8_t peloton_rlut_translate(uint16_t raw)
{
    if (raw <= m_rlut[0]) return 0;
    if (raw >= m_rlut[PELOTON_RLUT_ENTRIES - 1]) return 100;

    uint8_t lb = 0;
    while (lb < PELOTON_RLUT_ENTRIES - 2 && raw > m_rlut[lb + 1]) lb++;

    float span = (float)(m_rlut[lb + 1] - m_rlut[lb]);
    float frac = (span > 0.0f) ? (float)(raw - m_rlut[lb]) / span : 0.0f;
    float pct  = (100.0f / 30.0f) * ((float)lb + frac) + 0.5f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)pct;
}

/* One F7 entry arrived. The reply carries no index, so we rely on the
 * head unit reading 00..1E in order as one burst. */
static void rlut_feed(uint16_t value, uint32_t now)
{
    if (m_rlut_pending_n > 0 &&
        ticks_since(m_rlut_pending_ts, now) > ms_to_ticks(PELOTON_RLUT_GAP_MS))
    {
        m_rlut_pending_n = 0;
    }
    if (m_rlut_pending_n > 0 && value <= m_rlut_pending[m_rlut_pending_n - 1])
    {
        m_rlut_pending_n = 0;   /* not ascending: we joined mid-burst */
    }
    m_rlut_pending_ts = now;
    m_rlut_pending[m_rlut_pending_n++] = value;

    if (m_rlut_pending_n < PELOTON_RLUT_ENTRIES) return;

    memcpy(m_rlut, m_rlut_pending, sizeof(m_rlut));
    m_rlut_captured  = true;
    m_rlut_pending_n = 0;

    /* Re-translate the current raw resistance with the new table */
    if (m_ride.resistance_valid)
    {
        m_ride.resistance_pct = peloton_rlut_translate(m_ride.resistance_raw);
    }
}

bool             peloton_rlut_captured(void) { return m_rlut_captured; }
const uint16_t * peloton_rlut(void)          { return m_rlut; }

/*-------------------------------------------------------------------
 * Ride state updates
 *-----------------------------------------------------------------*/

static void update_cadence(uint16_t rpm, uint32_t now)
{
    if (!m_ride.cadence_valid ||
        ticks_since(m_ride.cadence_ts, now) > ms_to_ticks(PELOTON_DATA_TIMEOUT_MS))
    {
        /* First sample or stale: restart the integration from here */
        m_ride.cadence_ts    = now;
        m_ride.last_crank_ts = now;
    }

    uint32_t elapsed      = ticks_since(m_ride.cadence_ts, now);
    float    rev_per_tick = (float)rpm / 60.0f / (float)m_ticks_per_sec;
    uint32_t prev_whole   = (uint32_t)m_ride.crank_revs;

    m_ride.crank_revs += rev_per_tick * (float)elapsed;

    uint32_t new_whole = (uint32_t)m_ride.crank_revs;
    if (new_whole != prev_whole && rev_per_tick > 0.0f)
    {
        /* Back-date the event to when the last whole revolution completed */
        float partial = m_ride.crank_revs - (float)new_whole;
        m_ride.last_crank_ts = (now - (uint32_t)(partial / rev_per_tick)) & PELOTON_TICK_MASK;
    }
    if (m_ride.crank_revs >= 65536.0f)
    {
        m_ride.crank_revs -= 65536.0f;   /* the uint16 wire value wraps anyway */
    }

    m_ride.cadence_rpm   = rpm;
    m_ride.cadence_ts    = now;
    m_ride.cadence_valid = true;
}

static uint16_t calibrate(uint16_t raw_dw)
{
    int32_t cal = ((int32_t)raw_dw * (int32_t)m_cal_scale + 500) / 1000 + (int32_t)m_cal_offset;
    if (cal < 0)     cal = 0;
    if (cal > 65535) cal = 65535;
    return (uint16_t)cal;
}

static void update_power(uint16_t deciwatts, uint32_t now)
{
    deciwatts = calibrate(deciwatts);   /* the screen shows calibrated power; speed follows it */
    if (!m_ride.power_valid ||
        ticks_since(m_ride.power_ts, now) > ms_to_ticks(PELOTON_DATA_TIMEOUT_MS))
    {
        m_ride.power_ts      = now;
        m_ride.last_wheel_ts = now;
    }

    /* Integrate wheel revolutions from the power-derived speed over the
     * interval since the previous power sample (same scheme as the crank). */
    uint32_t elapsed = ticks_since(m_ride.power_ts, now);
    float    kmh     = peloton_speed_mph_from_power(m_ride.power_dw) * 1.609344f;
    float    rev_per_tick = (kmh / 3.6f) / m_wheel_m / (float)m_ticks_per_sec;
    uint32_t prev_whole = (uint32_t)m_ride.wheel_revs;

    m_ride.wheel_revs += rev_per_tick * (float)elapsed;

    uint32_t new_whole = (uint32_t)m_ride.wheel_revs;
    if (new_whole != prev_whole && rev_per_tick > 0.0f)
    {
        float partial = m_ride.wheel_revs - (float)new_whole;
        m_ride.last_wheel_ts = (now - (uint32_t)(partial / rev_per_tick)) & PELOTON_TICK_MASK;
    }
    if (m_ride.wheel_revs >= 16777216.0f)
    {
        m_ride.wheel_revs -= 16777216.0f;   /* keep float precision; uint32 on the wire */
    }

    m_ride.power_dw    = deciwatts;
    m_ride.power_ts    = now;
    m_ride.power_valid = true;
}

static void update_resistance(uint16_t raw, uint32_t now)
{
    m_ride.resistance_raw   = raw;
    if (!m_ride.res_level_from_bike)
    {
        m_ride.resistance_pct = peloton_rlut_translate(raw);
    }
    m_ride.resistance_ts    = now;
    m_ride.resistance_valid = true;
}

static void update_res_level(uint16_t pct, uint32_t now)
{
    m_ride.resistance_pct      = (pct > 100) ? 100 : (uint8_t)pct;
    m_ride.res_level_from_bike = true;
    m_ride.resistance_ts       = now;
    m_ride.resistance_valid    = true;
}

void peloton_expire_stale(uint32_t now)
{
    uint32_t limit = ms_to_ticks(PELOTON_DATA_TIMEOUT_MS);
    if (m_ride.cadence_valid    && ticks_since(m_ride.cadence_ts, now)    > limit) m_ride.cadence_valid    = false;
    if (m_ride.power_valid      && ticks_since(m_ride.power_ts, now)      > limit) m_ride.power_valid      = false;
    if (m_ride.resistance_valid && ticks_since(m_ride.resistance_ts, now) > limit)
    {
        m_ride.resistance_valid    = false;
        m_ride.res_level_from_bike = false;
    }
}

/*-------------------------------------------------------------------
 * Frame dispatch
 *-----------------------------------------------------------------*/

static void dispatch(uint8_t id, const uint8_t * digits, uint8_t len, uint32_t now)
{
    uint32_t value = 0;

    if (id == PELOTON_ID_BIKE_ID || id == PELOTON_ID_INIT)
    {
        /* Head-unit boot chatter; payload is not numeric for 0xFB */
        m_rlut_pending_n = 0;
        m_ride.frames_ok++;
        return;
    }

    /* ASCII digits, least significant first */
    for (int8_t i = (int8_t)len - 1; i >= 0; i--)
    {
        uint8_t d = (uint8_t)(digits[i] - '0');
        if (d > 9) { m_ride.frames_bad++; m_ride.bad_digit++; return; }
        value = value * 10 + d;
        if (value > 65535u) { m_ride.frames_bad++; m_ride.bad_digit++; return; }
    }

    switch (id)
    {
        case PELOTON_ID_CADENCE:
            if (len != 3) break;
            update_cadence((uint16_t)value, now);
            m_rlut_pending_n = 0;
            m_ride.frames_ok++;
            return;

        case PELOTON_ID_POWER:
            if (len != 5) break;
            update_power((uint16_t)value, now);
            m_rlut_pending_n = 0;
            m_ride.frames_ok++;
            return;

        case PELOTON_ID_RESISTANCE:
            if (len != 4) break;
            update_resistance((uint16_t)value, now);
            m_rlut_pending_n = 0;
            m_ride.frames_ok++;
            return;

        case PELOTON_ID_RES_LEVEL:
            if (len != 3) break;
            update_res_level((uint16_t)value, now);
            m_rlut_pending_n = 0;
            m_ride.frames_ok++;
            return;

        case PELOTON_ID_RLUT:
            if (len != 4) break;
            rlut_feed((uint16_t)value, now);
            m_ride.frames_ok++;
            return;

        default:
            break;
    }
    m_ride.frames_bad++;
    m_ride.bad_id++;
}

/*-------------------------------------------------------------------
 * Byte-level parser
 *-----------------------------------------------------------------*/

void peloton_resync(void)
{
    m_state = ST_WAIT_HEADER;
}

void peloton_parse_byte(uint8_t byte, uint32_t now)
{
    /* A header byte restarts a frame, which is how we fall back into sync
     * after noise or a dropped byte. The one exception is when 0xF1 is the
     * checksum we are expecting (same rule PeloMon uses). */
    if (byte == PELOTON_HEADER && !(m_state == ST_CHECKSUM && m_cs == PELOTON_HEADER))
    {
        m_cs    = byte;
        m_state = ST_ID;
        return;
    }

    switch (m_state)
    {
        case ST_WAIT_HEADER:
            break;

        case ST_ID:
            m_id    = byte;
            m_cs   += byte;
            m_state = ST_LEN;
            break;

        case ST_LEN:
            m_len = byte;
            m_cs += byte;
            m_idx = 0;
            if (m_len == 0 || m_len > PELOTON_MAX_PAYLOAD)
            {
                m_ride.frames_bad++;
                m_ride.bad_len++;
                m_state = ST_WAIT_HEADER;
            }
            else
            {
                m_state = ST_DATA;
            }
            break;

        case ST_DATA:
            m_data[m_idx++] = byte;
            m_cs += byte;
            if (m_idx >= m_len) m_state = ST_CHECKSUM;
            break;

        case ST_CHECKSUM:
            m_cs_ok = (m_cs == byte);
            m_state = ST_FOOTER;
            break;

        case ST_FOOTER:
            if (byte == PELOTON_FOOTER && m_cs_ok)
            {
                dispatch(m_id, m_data, m_len, now);
            }
            else
            {
                m_ride.frames_bad++;
                if (!m_cs_ok) m_ride.bad_cs++; else m_ride.bad_footer++;
            }
            m_state = ST_WAIT_HEADER;
            break;

        default:
            m_state = ST_WAIT_HEADER;
            break;
    }
}

/*-------------------------------------------------------------------
 * Accessors / derived values
 *-----------------------------------------------------------------*/

void peloton_init(uint32_t ticks_per_sec)
{
    memset(&m_ride, 0, sizeof(m_ride));
    m_ticks_per_sec = ticks_per_sec ? ticks_per_sec : 1000;
    m_state = ST_WAIT_HEADER;
    rlut_load_default();
}

const peloton_ride_t * peloton_ride(void) { return &m_ride; }

uint16_t peloton_watts(void)
{
    if (!m_ride.power_valid) return 0;
    return (uint16_t)((m_ride.power_dw + 5u) / 10u);
}

uint16_t peloton_cadence(void)
{
    return m_ride.cadence_valid ? m_ride.cadence_rpm : 0;
}

uint8_t peloton_resistance_pct(void)
{
    return m_ride.resistance_valid ? m_ride.resistance_pct : 0;
}

uint16_t peloton_crank_revs(void)
{
    return (uint16_t)m_ride.crank_revs;
}

uint32_t peloton_wheel_revs(void)
{
    return (uint32_t)m_ride.wheel_revs;
}

uint16_t peloton_wheel_event_time(void)
{
    return (uint16_t)(((uint64_t)m_ride.last_wheel_ts * 2048u) / m_ticks_per_sec);
}

uint16_t peloton_wheel_event_time_1024(void)
{
    return (uint16_t)(((uint64_t)m_ride.last_wheel_ts * 1024u) / m_ticks_per_sec);
}

uint16_t peloton_crank_event_time(void)
{
    /* 1/1024 s units, free-running, wraps at 64 s like the spec expects */
    return (uint16_t)(((uint64_t)m_ride.last_crank_ts * 1024u) / m_ticks_per_sec);
}

/* Speed as the Peloton head unit displays it: a function of power only.
 * Two cubics in sqrt(W) with a break at 27 W, fitted on 2026-10-14 by
 * scripts/fit_speed.py from 87 of the owner's own rides (2016-2026,
 * 45 685 per-second (W, mph) samples, 1-499 W, median speed per watt):
 * RMS 0.034 mph against the per-watt medians, 0.046 mph against the
 * samples, i.e. the display's 0.1 mph rounding. The functional form
 * follows PeloMon's observation (ihaque.org, 2020); the coefficients
 * are ours. Re-fit: python3 scripts/fit_speed.py fit <captures CSVs> */
float peloton_speed_mph_from_power(uint16_t deciwatts)
{
    if (deciwatts == 0) return 0.0f;   /* not pedaling: the fit's intercept is not zero */
    float power = (float)deciwatts * 0.1f;
    float rt    = sqrtf(power);
    static const float coefs_low[4]  = {-0.0672302f, 0.653241f, 0.121803f, -0.163307f};
    static const float coefs_high[4] = {0.000976495f, -0.0606787f, 2.27964f, -1.46339f};
    const float * c = (power < 27.0f) ? coefs_low : coefs_high;
    float mph = 0.0f;
    for (uint8_t i = 0; i < 3; i++)
    {
        mph += c[i];
        mph *= rt;
    }
    mph += c[3];
    return (mph > 0.0f) ? mph : 0.0f;
}

void peloton_set_power_calibration(uint16_t scale_x1000, int16_t offset_dw)
{
    m_cal_scale  = scale_x1000 ? scale_x1000 : 1000;
    m_cal_offset = offset_dw;
}

void peloton_set_wheel_circumference_mm(uint16_t mm)
{
    if (mm) m_wheel_m = (float)mm / 1000.0f;
}

uint16_t peloton_power_scale_x1000(void)      { return m_cal_scale; }
int16_t  peloton_power_offset_dw(void)        { return m_cal_offset; }
uint16_t peloton_wheel_circumference_mm(void) { return (uint16_t)(m_wheel_m * 1000.0f + 0.5f); }

uint16_t peloton_speed_kmh_x100(bool from_power, float wheel_circumference_m, float gear_ratio)
{
    float kmh;
    if (from_power)
    {
        if (!m_ride.power_valid) return 0;
        kmh = peloton_speed_mph_from_power(m_ride.power_dw) * 1.609344f;
    }
    else
    {
        if (!m_ride.cadence_valid) return 0;
        kmh = (float)m_ride.cadence_rpm * wheel_circumference_m * gear_ratio * 60.0f / 1000.0f;
    }
    float v = kmh * 100.0f + 0.5f;
    if (v > 65535.0f) v = 65535.0f;
    return (uint16_t)v;
}
