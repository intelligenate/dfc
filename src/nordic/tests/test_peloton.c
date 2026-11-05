/* Host unit test for peloton.c — build & run with:
 *   cc -std=c11 -Wall -Wextra -I.. -o test_peloton test_peloton.c ../peloton.c -lm && ./test_peloton [example-ride.txt]
 */
#include "peloton.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TPS 16384u                         /* nRF app_timer: 32768 Hz / (prescaler 1 + 1) */
#define MS(ms) ((uint32_t)((uint64_t)(ms) * TPS / 1000))

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Build a bike frame: F1 id len <digits LSB first> cs F6 */
static size_t frame(uint8_t * out, uint8_t id, uint8_t len, uint32_t value)
{
    size_t n = 0;
    out[n++] = 0xF1; out[n++] = id; out[n++] = len;
    for (uint8_t i = 0; i < len; i++) { out[n++] = (uint8_t)('0' + value % 10); value /= 10; }
    uint8_t cs = 0; for (size_t i = 0; i < n; i++) cs += out[i];
    out[n++] = cs; out[n++] = 0xF6;
    return n;
}

static void feed(const uint8_t * b, size_t n, uint32_t now)
{
    for (size_t i = 0; i < n; i++) peloton_parse_byte(b[i], now);
}

static void send(uint8_t id, uint8_t len, uint32_t value, uint32_t now)
{
    uint8_t b[16]; size_t n = frame(b, id, len, value); feed(b, n, now);
}

static void test_basic_decode(void)
{
    printf("basic decode\n");
    peloton_init(TPS);
    /* PeloMon examples: cadence 084 -> 34 38 30, power 0155.0 -> 30 35 35 31 30 */
    const uint8_t cad[] = {0xF1, 0x41, 0x03, 0x34, 0x38, 0x30, 0x00, 0xF6};
    uint8_t c[8]; memcpy(c, cad, 8); { uint8_t cs = 0; for (int i = 0; i < 6; i++) cs += c[i]; c[6] = cs; }
    feed(c, 8, MS(100));
    CHECK(peloton_cadence() == 84, "cadence %u != 84", peloton_cadence());

    const uint8_t pw[] = {0xF1, 0x44, 0x05, 0x30, 0x35, 0x35, 0x31, 0x30, 0x00, 0xF6};
    uint8_t p[10]; memcpy(p, pw, 10); { uint8_t cs = 0; for (int i = 0; i < 8; i++) cs += p[i]; p[8] = cs; }
    feed(p, 10, MS(200));
    CHECK(peloton_ride()->power_dw == 1550, "power_dw %u != 1550", peloton_ride()->power_dw);
    CHECK(peloton_watts() == 155, "watts %u != 155", peloton_watts());

    send(PELOTON_ID_RESISTANCE, 4, 668, MS(300));
    CHECK(peloton_ride()->resistance_raw == 668, "raw %u", peloton_ride()->resistance_raw);
    /* default table (owner's bike): 663 (idx 11) < 668 < 698 (idx 12) -> 11.14 * 3.33 = 37.1 -> 37 */
    CHECK(peloton_resistance_pct() == 37, "pct %u != 37", peloton_resistance_pct());
    CHECK(peloton_ride()->frames_ok == 3 && peloton_ride()->frames_bad == 0, "ok=%u bad=%u",
          peloton_ride()->frames_ok, peloton_ride()->frames_bad);

    /* rounding of deciwatts */
    send(PELOTON_ID_POWER, 5, 1555, MS(400)); CHECK(peloton_watts() == 156, "round 155.5 -> %u", peloton_watts());
    send(PELOTON_ID_POWER, 5, 1554, MS(500)); CHECK(peloton_watts() == 155, "round 155.4 -> %u", peloton_watts());
}

static void test_capture_2026_10_12(void)
{
    printf("real capture (power, raw res, res level 0x49, cadence)\n");
    peloton_init(TPS);
    const uint8_t cap[] = {
        0xF1,0x44,0x05,0x39,0x31,0x31,0x30,0x30,0x35,0xF6,   /* power 00119 dw */
        0xF1,0x4A,0x04,0x36,0x35,0x34,0x30,0x0E,0xF6,        /* raw resistance 0456 */
        0xF1,0x49,0x03,0x32,0x32,0x30,0xD1,0xF6,             /* resistance level 022 */
        0xF1,0x41,0x03,0x39,0x34,0x30,0xD2,0xF6 };           /* cadence 049 */
    feed(cap, sizeof(cap), MS(100));
    CHECK(peloton_ride()->frames_ok == 4 && peloton_ride()->frames_bad == 0, "ok=%u bad=%u", peloton_ride()->frames_ok, peloton_ride()->frames_bad);
    CHECK(peloton_watts() == 12, "watts %u != 12", peloton_watts());
    CHECK(peloton_ride()->resistance_raw == 456, "raw %u", peloton_ride()->resistance_raw);
    CHECK(peloton_resistance_pct() == 22, "pct %u != 22 (bike-reported)", peloton_resistance_pct());
    CHECK(peloton_cadence() == 49, "cadence %u", peloton_cadence());
    /* a later raw-only frame must not overwrite the bike-reported level */
    send(PELOTON_ID_RESISTANCE, 4, 900, MS(400));
    CHECK(peloton_resistance_pct() == 22, "table overrode bike level: %u", peloton_resistance_pct());
    /* after a timeout the fallback to the table returns */
    peloton_expire_stale(MS(7000));
    send(PELOTON_ID_RESISTANCE, 4, 900, MS(7100));
    CHECK(peloton_resistance_pct() != 22 && peloton_resistance_pct() > 50, "fallback pct %u", peloton_resistance_pct());
}

static void test_bad_frames_and_resync(void)
{
    printf("bad frames + resync\n");
    peloton_init(TPS);
    send(PELOTON_ID_CADENCE, 3, 80, MS(0));
    uint8_t b[16]; size_t n = frame(b, PELOTON_ID_CADENCE, 3, 95);
    b[n - 2] ^= 0x01;                      /* corrupt checksum */
    feed(b, n, MS(300));
    CHECK(peloton_cadence() == 80, "bad checksum changed cadence to %u", peloton_cadence());
    CHECK(peloton_ride()->frames_bad == 1, "bad=%u", peloton_ride()->frames_bad);

    n = frame(b, PELOTON_ID_CADENCE, 3, 95);
    b[n - 1] = 0x00;                       /* bad footer */
    feed(b, n, MS(600));
    CHECK(peloton_cadence() == 80 && peloton_ride()->frames_bad == 2, "bad footer accepted");

    /* noise, partial frame, then a good one */
    const uint8_t noise[] = {0x00, 0xFF, 0xF1, 0x41, 0x03, 0x31};   /* truncated */
    feed(noise, sizeof(noise), MS(900));
    send(PELOTON_ID_CADENCE, 3, 95, MS(1000));
    CHECK(peloton_cadence() == 95, "did not resync, cadence %u", peloton_cadence());

    /* head-unit frames must never be mistaken for data */
    const uint8_t hu[] = {0xF5, 0x41, 0x36, 0xF6};
    feed(hu, 4, MS(1100));
    CHECK(peloton_cadence() == 95 && peloton_ride()->frames_bad == 2, "HU request disturbed state");

    /* wrong digit count for an id */
    send(PELOTON_ID_CADENCE, 4, 95, MS(1200));
    CHECK(peloton_ride()->frames_bad == 3, "len mismatch not rejected");

    /* boot chatter: bike ID with non-digit payload, init reply */
    const uint8_t bid[] = {0xF1, 0xFB, 0x07, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 0x00, 0xF6};
    uint8_t bb[12]; memcpy(bb, bid, 12); { uint8_t cs = 0; for (int i = 0; i < 10; i++) cs += bb[i]; bb[10] = cs; }
    feed(bb, 12, MS(1300));
    send(PELOTON_ID_INIT, 3, 0, MS(1400));
    CHECK(peloton_ride()->frames_ok == 4, "boot frames ok=%u (cadence x2 + bike id + init)", peloton_ride()->frames_ok);

    /* a frame whose checksum byte is itself 0xF1 must still decode */
    uint32_t found = 0;
    for (uint32_t v = 0; v < 1000 && !found; v++) { uint8_t t[16]; size_t k = frame(t, PELOTON_ID_CADENCE, 3, v); if (t[k - 2] == 0xF1) found = v; }
    if (found) { send(PELOTON_ID_CADENCE, 3, found, MS(2000)); CHECK(peloton_cadence() == found, "cs==F1 frame lost (v=%u got %u)", found, peloton_cadence()); }
}

static void test_rlut_capture(void)
{
    printf("resistance table capture\n");
    peloton_init(TPS);
    CHECK(!peloton_rlut_captured(), "captured before any data");
    CHECK(peloton_rlut_translate(0) == 0 && peloton_rlut_translate(5000) == 100, "default endpoints");

    /* A fake table: 100, 130, 160, ... (31 entries) */
    uint32_t t = MS(0);
    for (int i = 0; i < 31; i++) { send(PELOTON_ID_RLUT, 4, 100 + 30 * i, t); t += MS(200); }
    CHECK(peloton_rlut_captured(), "table not captured");
    CHECK(peloton_rlut()[0] == 100 && peloton_rlut()[30] == 1000, "table contents %u..%u", peloton_rlut()[0], peloton_rlut()[30]);
    CHECK(peloton_rlut_translate(100) == 0, "lo");
    CHECK(peloton_rlut_translate(1000) == 100, "hi");
    CHECK(peloton_rlut_translate(550) == 50, "mid -> %u", peloton_rlut_translate(550));
    CHECK(peloton_rlut_translate(115) == 2, "entry 0.5 -> %u (expect ~1.67 -> 2)", peloton_rlut_translate(115));

    /* Joining mid-burst must not produce a bogus table */
    peloton_init(TPS);
    t = MS(0);
    for (int i = 10; i < 31; i++) { send(PELOTON_ID_RLUT, 4, 100 + 30 * i, t); t += MS(200); }
    for (int i = 0; i < 10; i++)  { send(PELOTON_ID_RLUT, 4, 100 + 30 * i, t); t += MS(200); }
    CHECK(!peloton_rlut_captured(), "non-monotonic burst accepted");

    /* A gap resets the burst */
    peloton_init(TPS);
    t = MS(0);
    for (int i = 0; i < 20; i++) { send(PELOTON_ID_RLUT, 4, 100 + 30 * i, t); t += MS(200); }
    t += MS(5000);
    for (int i = 20; i < 31; i++) { send(PELOTON_ID_RLUT, 4, 100 + 30 * i, t); t += MS(200); }
    CHECK(!peloton_rlut_captured(), "gap-split burst accepted");

    /* A ride message resets the burst too */
    peloton_init(TPS);
    t = MS(0);
    for (int i = 0; i < 20; i++) { send(PELOTON_ID_RLUT, 4, 100 + 30 * i, t); t += MS(200); }
    send(PELOTON_ID_CADENCE, 3, 0, t);
    for (int i = 20; i < 31; i++) { send(PELOTON_ID_RLUT, 4, 100 + 30 * i, t); t += MS(200); }
    CHECK(!peloton_rlut_captured(), "interrupted burst accepted");
}

static void test_crank_synthesis(void)
{
    printf("crank revolution synthesis\n");
    peloton_init(TPS);
    /* 60 rpm reported every 300 ms for 30 s: expect ~30 revs, and the
     * cadence a client computes from (revs, event time) deltas to be 60. */
    uint32_t t = MS(1000);
    uint16_t revs_a = 0, time_a = 0;
    for (int i = 0; i <= 100; i++)
    {
        send(PELOTON_ID_CADENCE, 3, 60, t);
        if (i == 50) { revs_a = peloton_crank_revs(); time_a = peloton_crank_event_time(); }
        t += MS(300);
    }
    uint16_t revs_b = peloton_crank_revs(), time_b = peloton_crank_event_time();
    CHECK(revs_b >= 29 && revs_b <= 31, "revs after 30 s = %u", revs_b);
    uint16_t drev = (uint16_t)(revs_b - revs_a);
    uint16_t dt   = (uint16_t)(time_b - time_a);
    float rpm = dt ? (float)drev * 60.0f * 1024.0f / (float)dt : 0;
    CHECK(fabsf(rpm - 60.0f) < 1.0f, "client-derived cadence %.2f != 60 (drev=%u dt=%u)", rpm, drev, dt);

    /* stop pedaling: counters freeze */
    send(PELOTON_ID_CADENCE, 3, 0, t); t += MS(300);
    uint16_t r1 = peloton_crank_revs(), e1 = peloton_crank_event_time();
    for (int i = 0; i < 10; i++) { send(PELOTON_ID_CADENCE, 3, 0, t); t += MS(300); }
    CHECK(peloton_crank_revs() == r1 && peloton_crank_event_time() == e1, "counters moved at 0 rpm");

    /* tick wrap (24-bit RTC): keep integrating across the wrap */
    peloton_init(TPS);
    t = 0x00FFFFFFu - MS(1000);
    for (int i = 0; i < 20; i++) { send(PELOTON_ID_CADENCE, 3, 90, t); t = (t + MS(300)) & 0x00FFFFFFu; }
    CHECK(peloton_crank_revs() >= 8 && peloton_crank_revs() <= 10, "revs across wrap = %u", peloton_crank_revs());
}

static void test_wheel_synthesis(void)
{
    printf("wheel revolution synthesis from power\n");
    peloton_init(TPS);
    /* 100 W steady for 30 s, power every 300 ms. Expected speed = regression(100 W). */
    float kmh = peloton_speed_mph_from_power(1000) * 1.609344f;
    uint32_t t = MS(1000);
    uint32_t revs_a = 0; uint16_t time_a = 0;
    for (int i = 0; i <= 100; i++)
    {
        send(PELOTON_ID_POWER, 5, 1000, t);
        if (i == 50) { revs_a = peloton_wheel_revs(); time_a = peloton_wheel_event_time(); }
        t += MS(300);
    }
    uint32_t revs_b = peloton_wheel_revs(); uint16_t time_b = peloton_wheel_event_time();
    float expected_revs = (kmh / 3.6f) / 2.105f * 30.0f;
    CHECK(fabsf((float)revs_b - expected_revs) < 2.0f, "revs after 30 s = %u, expected ~%.1f", revs_b, expected_revs);
    uint32_t drev = revs_b - revs_a; uint16_t dt = (uint16_t)(time_b - time_a);
    float client_kmh = dt ? (float)drev * 2.105f / ((float)dt / 2048.0f) * 3.6f : 0;
    CHECK(fabsf(client_kmh - kmh) < 0.3f, "client-derived speed %.2f km/h != %.2f", client_kmh, kmh);
    printf("  100 W -> %.1f km/h, %u wheel revs in 30 s\n", kmh, revs_b);
    /* 0 W: wheel stops */
    send(PELOTON_ID_POWER, 5, 0, t); t += MS(300);
    uint32_t r1 = peloton_wheel_revs(); uint16_t e1 = peloton_wheel_event_time();
    for (int i = 0; i < 10; i++) { send(PELOTON_ID_POWER, 5, 0, t); t += MS(300); }
    CHECK(peloton_wheel_revs() == r1 && peloton_wheel_event_time() == e1, "wheel moved at 0 W");
}

static void test_stale(void)
{
    printf("stale expiry\n");
    peloton_init(TPS);
    send(PELOTON_ID_CADENCE, 3, 70, MS(0));
    send(PELOTON_ID_POWER, 5, 2000, MS(0));
    send(PELOTON_ID_RESISTANCE, 4, 500, MS(0));
    peloton_expire_stale(MS(4000));
    CHECK(peloton_cadence() == 70 && peloton_watts() == 200, "expired too early");
    peloton_expire_stale(MS(6000));
    CHECK(peloton_cadence() == 0 && peloton_watts() == 0 && peloton_resistance_pct() == 0, "did not expire");
    CHECK(peloton_speed_kmh_x100(true, 2.105f, 3.5f) == 0, "speed not zero when stale");
    /* comes back with new data, and the crank integration restarts cleanly */
    send(PELOTON_ID_CADENCE, 3, 70, MS(60000));
    CHECK(peloton_cadence() == 70, "did not recover");
}

static void test_speed(void)
{
    printf("speed\n");
    peloton_init(TPS);
    float m100 = peloton_speed_mph_from_power(1000);   /* 100 W */
    float m200 = peloton_speed_mph_from_power(2000);
    float m10  = peloton_speed_mph_from_power(100);
    CHECK(m100 > 10 && m100 < 25, "100 W -> %.1f mph", m100);
    CHECK(m200 > m100, "monotonic");
    CHECK(m10 >= 0 && m10 < m100, "low power branch %.2f", m10);
    CHECK(peloton_speed_mph_from_power(0) == 0.0f, "0 W -> %.2f", peloton_speed_mph_from_power(0));
    /* points of the 2026-10-14 fit (median displayed speed per watt, 87 rides) */
    static const struct { uint16_t dw; float mph; } fit[] = {
        {100, 4.63f}, {200, 7.43f}, {500, 11.97f}, {1000, 16.24f}, {2000, 21.40f}, {3000, 24.89f}
    };
    for (unsigned i = 0; i < sizeof fit / sizeof fit[0]; i++)
    {
        float v = peloton_speed_mph_from_power(fit[i].dw);
        CHECK(fabsf(v - fit[i].mph) < 0.15f, "%u dW -> %.2f mph (fit %.2f)", fit[i].dw, v, fit[i].mph);
    }
    {
        float prev = 0.0f; bool mono = true;
        for (uint16_t w = 1; w <= 600; w++)
        {
            float v = peloton_speed_mph_from_power(w * 10);
            if (v < prev) { mono = false; break; }
            prev = v;
        }
        CHECK(mono, "curve non-decreasing 1-600 W");
    }
    send(PELOTON_ID_POWER, 5, 1000, MS(0));
    uint16_t kmh100 = peloton_speed_kmh_x100(true, 2.105f, 3.5f);
    CHECK(fabsf((float)kmh100 / 100.0f - m100 * 1.609344f) < 0.02f, "km/h conversion %u", kmh100);
    send(PELOTON_ID_CADENCE, 3, 80, MS(0));
    uint16_t legacy = peloton_speed_kmh_x100(false, 2.105f, 3.5f);
    CHECK(abs((int)legacy - 3536) <= 1, "cadence speed %u != 3536", legacy);
    printf("  100 W -> %.1f mph, 200 W -> %.1f mph\n", m100, m200);
}

/* Optional: replay PeloMon's example-ride.txt (12 payload digits per line:
 * cadence[3] power[5] resistance[4]) as full frames. */
static void replay(const char * path)
{
    FILE * f = fopen(path, "r");
    if (!f) { printf("replay: cannot open %s\n", path); return; }
    peloton_init(TPS);
    char line[256]; uint32_t t = 0; unsigned lines = 0; uint16_t max_c = 0, max_w = 0, max_r = 0, min_r = 100;
    while (fgets(line, sizeof(line), f))
    {
        uint8_t d[12]; int n = 0; char * s = line;
        while (n < 12) { char * e; long v = strtol(s, &e, 16); if (e == s) break; d[n++] = (uint8_t)v; s = e; }
        if (n != 12) continue;
        uint8_t b[16]; size_t k;
        k = 0; b[k++] = 0xF1; b[k++] = 0x41; b[k++] = 3; memcpy(b + k, d, 3); k += 3;
        { uint8_t cs = 0; for (size_t i = 0; i < k; i++) cs += b[i]; b[k++] = cs; } b[k++] = 0xF6; feed(b, k, t); t += MS(100);
        k = 0; b[k++] = 0xF1; b[k++] = 0x44; b[k++] = 5; memcpy(b + k, d + 3, 5); k += 5;
        { uint8_t cs = 0; for (size_t i = 0; i < k; i++) cs += b[i]; b[k++] = cs; } b[k++] = 0xF6; feed(b, k, t); t += MS(100);
        k = 0; b[k++] = 0xF1; b[k++] = 0x4A; b[k++] = 4; memcpy(b + k, d + 8, 4); k += 4;
        { uint8_t cs = 0; for (size_t i = 0; i < k; i++) cs += b[i]; b[k++] = cs; } b[k++] = 0xF6; feed(b, k, t); t += MS(100);
        lines++;
        if (peloton_cadence() > max_c) max_c = peloton_cadence();
        if (peloton_watts() > max_w) max_w = peloton_watts();
        if (peloton_resistance_pct() > max_r) max_r = peloton_resistance_pct();
        if (peloton_resistance_pct() < min_r) min_r = peloton_resistance_pct();
    }
    fclose(f);
    printf("replay: %u lines -> ok=%u bad=%u, max cadence %u rpm, max power %u W, resistance %u..%u %%, crank revs %u\n",
           lines, peloton_ride()->frames_ok, peloton_ride()->frames_bad, max_c, max_w, min_r, max_r, peloton_crank_revs());
    CHECK(peloton_ride()->frames_ok == lines * 3 && peloton_ride()->frames_bad == 0, "replay frame errors");
}

/* Phase 5 settings hooks: power calibration and wheel circumference. */
static void test_calibration(void)
{
    printf("power calibration + wheel circumference\n");
    /* (d) defaults, and peloton_init must not touch them */
    peloton_set_power_calibration(1000, 0);
    peloton_set_wheel_circumference_mm(2105);
    peloton_init(TPS);
    CHECK(peloton_power_scale_x1000() == 1000 && peloton_power_offset_dw() == 0 && peloton_wheel_circumference_mm() == 2105,
          "defaults %u %d %u", peloton_power_scale_x1000(), peloton_power_offset_dw(), peloton_wheel_circumference_mm());
    send(0x44, 5, 1000, MS(100));
    CHECK(peloton_watts() == 100, "uncalibrated 1000 dW -> %u W", peloton_watts());

    /* (a) scale 1.10, offset -2.0 W: 1000 dW -> 1100 - 20 = 1080 dW = 108 W */
    peloton_set_power_calibration(1100, -20);
    peloton_init(TPS);
    CHECK(peloton_power_scale_x1000() == 1100 && peloton_power_offset_dw() == -20, "init reset the calibration");
    send(0x44, 5, 1000, MS(100));
    CHECK(peloton_watts() == 108, "calibrated 1000 dW -> %u W (want 108)", peloton_watts());
    /* rounding: 333 dW * 1.1 = 366.3 -> 366; -20 -> 346 dW -> 35 W */
    send(0x44, 5, 333, MS(200));
    CHECK(peloton_ride()->power_dw == 346, "rounding: %u dW (want 346)", peloton_ride()->power_dw);
    /* the FTMS/speed path sees calibrated power too */
    uint16_t kmh_cal = peloton_speed_kmh_x100(true, 2.105f, 1.0f);
    peloton_set_power_calibration(1000, 0);
    peloton_init(TPS);
    send(0x44, 5, 346, MS(100));
    CHECK(peloton_speed_kmh_x100(true, 2.105f, 1.0f) == kmh_cal, "speed from calibrated power");

    /* (b) offset below zero clamps at 0, never negative / wraps */
    peloton_set_power_calibration(1000, -500);
    peloton_init(TPS);
    send(0x44, 5, 300, MS(100));
    CHECK(peloton_watts() == 0 && peloton_ride()->power_dw == 0, "negative clamp: %u dW", peloton_ride()->power_dw);
    send(0x44, 5, 600, MS(200));
    CHECK(peloton_watts() == 10, "600 dW - 500 -> %u W (want 10)", peloton_watts());
    peloton_set_power_calibration(1000, 0);

    /* (c) wheel circumference changes the revolution count for the same speed */
    uint32_t revs[2]; uint16_t mm[2] = {2105, 2000};
    for (int k = 0; k < 2; k++)
    {
        peloton_set_wheel_circumference_mm(mm[k]);
        peloton_init(TPS);
        for (uint32_t t = 0; t <= 30000; t += 100) send(0x44, 5, 1500, MS(t));
        revs[k] = peloton_wheel_revs();
    }
    double ratio = (double)revs[1] / (double)revs[0];
    CHECK(revs[0] > 50 && fabs(ratio - 2105.0 / 2000.0) < 0.01, "wheel ratio %.4f (want 1.0525), revs %u / %u", ratio, revs[0], revs[1]);
    peloton_set_wheel_circumference_mm(2105);
    peloton_init(TPS);
}

int main(int argc, char ** argv)
{
    test_basic_decode();
    test_capture_2026_10_12();
    test_bad_frames_and_resync();
    test_rlut_capture();
    test_crank_synthesis();
    test_wheel_synthesis();
    test_stale();
    test_speed();
    test_calibration();
    if (argc > 1) replay(argv[1]);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
