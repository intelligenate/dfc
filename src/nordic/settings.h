/* Persisted owner settings (phase 5): power calibration, wheel circumference,
 * CSC mode. Stored as one FDS record in the app-data pages at 0xF5000, which
 * the bootloader preserves across OTA updates. Values are integers only
 * (FDS stores whole words; no floats on the wire).
 *
 * FDS is initialised by the Peer Manager (pm_init); settings_init() must run
 * after it. Loading may complete asynchronously (FDS_EVT_INIT); until then the
 * defaults are in effect and settings_source() reports "default". */
#ifndef DFC_SETTINGS_H
#define DFC_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "sdk_errors.h"

#define SETTINGS_FILE_ID        0x0DFC   /* below the Peer Manager range (0xC000+) */
#define SETTINGS_RECORD_KEY     0x0001
#define SETTINGS_MAGIC          0x44464353UL   /* 'DFCS' */
#define SETTINGS_VERSION        1

#define SETTINGS_SCALE_DEFAULT  1000     /* x1000: 1000 = power unchanged */
#define SETTINGS_SCALE_MIN      500
#define SETTINGS_SCALE_MAX      2000
#define SETTINGS_OFFSET_DEFAULT 0        /* deciwatts, added after scaling */
#define SETTINGS_OFFSET_MIN     (-500)
#define SETTINGS_OFFSET_MAX     500
#define SETTINGS_WHEEL_DEFAULT  2105     /* mm, 700x25c */
#define SETTINGS_WHEEL_MIN      500
#define SETTINGS_WHEEL_MAX      3000
#define SETTINGS_CSC_DEFAULT    0        /* 0 CPS + wheel data, 1 CSC only, 2 both */
#define SETTINGS_CSC_MAX        2

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t power_scale_x1000;
    int16_t  power_offset_dw;
    uint16_t wheel_mm;
    uint8_t  csc_mode;
    uint8_t  reserved[5];
} dfc_settings_t;                        /* 16 bytes = 4 words */

/** Register with FDS and load the record (now, or on FDS_EVT_INIT). Applies
 *  the values to the decoder. Call after pm_init(). */
void settings_init(void);

const dfc_settings_t * settings_get(void);

/** "default" (no record), "fds" (loaded or saved), "ram" (changed, save
 *  pending or failed). */
const char * settings_source(void);

/** Setters validate, apply to the decoder and schedule a save. They return
 *  false and change nothing when a value is out of range. */
bool settings_set_power(int32_t scale_x1000, int32_t offset_dw);
bool settings_set_wheel(int32_t mm);
bool settings_set_csc(int32_t mode);

/** Back to defaults, delete the record. */
void settings_reset(void);

#endif
