#include "settings.h"
#include <string.h>
#include "fds.h"
#include "peloton.h"
#include "nrf_log.h"
#include "nrf_delay.h"

typedef enum { SRC_DEFAULT, SRC_FDS, SRC_RAM } src_t;

static dfc_settings_t m_set;                 /* live values */
static dfc_settings_t m_rec_data;            /* copy handed to FDS; must stay valid until the op completes */
static src_t          m_src       = SRC_DEFAULT;
static volatile bool  m_loaded    = false;   /* record read once (set from the FDS event) */
static bool           m_save_pend = false;   /* a save is wanted */
static bool           m_op_busy   = false;   /* write/update/delete queued, waiting for its event */
static bool           m_delete_pend = false;

static void set_defaults(void)
{
    memset(&m_set, 0, sizeof(m_set));
    m_set.magic             = SETTINGS_MAGIC;
    m_set.version           = SETTINGS_VERSION;
    m_set.power_scale_x1000 = SETTINGS_SCALE_DEFAULT;
    m_set.power_offset_dw   = SETTINGS_OFFSET_DEFAULT;
    m_set.wheel_mm          = SETTINGS_WHEEL_DEFAULT;
    m_set.csc_mode          = SETTINGS_CSC_DEFAULT;
}

static bool in_range(const dfc_settings_t * s)
{
    return s->magic == SETTINGS_MAGIC && s->version == SETTINGS_VERSION &&
           s->power_scale_x1000 >= SETTINGS_SCALE_MIN && s->power_scale_x1000 <= SETTINGS_SCALE_MAX &&
           s->power_offset_dw >= SETTINGS_OFFSET_MIN && s->power_offset_dw <= SETTINGS_OFFSET_MAX &&
           s->wheel_mm >= SETTINGS_WHEEL_MIN && s->wheel_mm <= SETTINGS_WHEEL_MAX &&
           s->csc_mode <= SETTINGS_CSC_MAX;
}

static void apply(void)
{
    peloton_set_power_calibration(m_set.power_scale_x1000, m_set.power_offset_dw);
    peloton_set_wheel_circumference_mm(m_set.wheel_mm);
}

/* Returns NRF_SUCCESS with desc filled when the record exists. */
static ret_code_t find(fds_record_desc_t * p_desc)
{
    fds_find_token_t tok;
    memset(&tok, 0, sizeof(tok));
    return fds_record_find(SETTINGS_FILE_ID, SETTINGS_RECORD_KEY, p_desc, &tok);
}

static void load(void)
{
    fds_record_desc_t desc;
    ret_code_t rc = find(&desc);
    if (rc == FDS_ERR_NOT_INITIALIZED) return;      /* FDS_EVT_INIT will call us again */
    m_loaded = true;
    if (rc != NRF_SUCCESS)
    {
        set_defaults(); m_src = SRC_DEFAULT; apply();
        return;
    }
    fds_flash_record_t frec;
    if (fds_record_open(&desc, &frec) == NRF_SUCCESS)
    {
        dfc_settings_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        uint32_t bytes = frec.p_header->length_words * 4u;
        memcpy(&tmp, frec.p_data, bytes < sizeof(tmp) ? bytes : sizeof(tmp));
        (void) fds_record_close(&desc);
        if (in_range(&tmp)) { m_set = tmp; m_src = SRC_FDS; }
        else                { set_defaults(); m_src = SRC_DEFAULT; }
    }
    else
    {
        set_defaults(); m_src = SRC_DEFAULT;
    }
    apply();
}

/* Queue the pending write/update/delete if FDS is idle for us. All errors
 * are logged and left for the next attempt (never a fault). */
static void kick(void)
{
    if (m_op_busy || !m_loaded) return;

    fds_record_desc_t desc;
    ret_code_t        rc;

    if (m_delete_pend)
    {
        if (find(&desc) == NRF_SUCCESS)
        {
            rc = fds_record_delete(&desc);
            if (rc == NRF_SUCCESS) { m_op_busy = true; return; }
            NRF_LOG_WARNING("settings: delete failed 0x%x", rc);
            if (rc == FDS_ERR_NO_SPACE_IN_QUEUES) return;   /* retry on the next event */
        }
        m_delete_pend = false;                              /* nothing to delete */
    }

    if (!m_save_pend) return;

    m_rec_data = m_set;
    fds_record_t rec =
    {
        .file_id           = SETTINGS_FILE_ID,
        .key               = SETTINGS_RECORD_KEY,
        .data.p_data       = &m_rec_data,
        .data.length_words = (sizeof(m_rec_data) + 3) / 4,
    };
    if (find(&desc) == NRF_SUCCESS) rc = fds_record_update(&desc, &rec);
    else                            rc = fds_record_write(&desc, &rec);

    if (rc == NRF_SUCCESS)
    {
        m_op_busy = true;
    }
    else if (rc == FDS_ERR_NO_SPACE_IN_FLASH)
    {
        NRF_LOG_WARNING("settings: flash full, gc");
        if (fds_gc() == NRF_SUCCESS) m_op_busy = true;      /* FDS_EVT_GC retries */
    }
    else
    {
        NRF_LOG_WARNING("settings: save failed 0x%x", rc);  /* stays pending, src=ram */
    }
}

static void fds_evt_handler(fds_evt_t const * p_evt)
{
    switch (p_evt->id)
    {
        case FDS_EVT_INIT:
            if (p_evt->result == NRF_SUCCESS && !m_loaded) load();
            kick();
            break;

        case FDS_EVT_WRITE:
        case FDS_EVT_UPDATE:
            if (p_evt->write.file_id != SETTINGS_FILE_ID) break;
            m_op_busy = false;
            if (p_evt->result == NRF_SUCCESS)
            {
                /* Only clear the pending flag if nothing changed meanwhile. */
                if (memcmp(&m_rec_data, &m_set, sizeof(m_set)) == 0) { m_save_pend = false; m_src = SRC_FDS; }
            }
            else
            {
                NRF_LOG_WARNING("settings: write result 0x%x", p_evt->result);
                if (p_evt->result == FDS_ERR_NO_SPACE_IN_FLASH && fds_gc() == NRF_SUCCESS) { m_op_busy = true; break; }
            }
            kick();
            break;

        case FDS_EVT_DEL_RECORD:
            if (p_evt->del.file_id != SETTINGS_FILE_ID) break;
            m_op_busy = false;
            if (p_evt->result == NRF_SUCCESS) m_delete_pend = false;
            kick();
            break;

        case FDS_EVT_GC:
            /* Also fired by the Peer Manager's gc requests. */
            m_op_busy = false;
            kick();
            break;

        default:
            break;
    }
}

void settings_init(void)
{
    set_defaults();
    apply();
    ret_code_t rc = fds_register(fds_evt_handler);
    if (rc != NRF_SUCCESS)
    {
        NRF_LOG_WARNING("settings: fds_register 0x%x", rc);   /* defaults stay in effect */
        return;
    }
    load();   /* no-op until FDS_EVT_INIT if FDS is still initialising */

    /* The CSC mode decides which services are created right after this, so
     * the record must be in before services_init(). FDS init is usually
     * already complete (pm_init ran it); on a fresh flash it tags pages
     * first and FDS_EVT_INIT arrives from the SoC event a few ms later. */
    for (int i = 0; i < 100 && !m_loaded; i++) nrf_delay_ms(5);
    if (!m_loaded) NRF_LOG_WARNING("settings: FDS not ready, defaults in effect");
}

const dfc_settings_t * settings_get(void)
{
    return &m_set;
}

const char * settings_source(void)
{
    switch (m_src)
    {
        case SRC_FDS: return "fds";
        case SRC_RAM: return "ram";
        default:      return "default";
    }
}

static void changed(void)
{
    apply();
    m_src         = SRC_RAM;
    m_save_pend   = true;
    m_delete_pend = false;
    kick();
}

bool settings_set_power(int32_t scale_x1000, int32_t offset_dw)
{
    if (scale_x1000 < SETTINGS_SCALE_MIN || scale_x1000 > SETTINGS_SCALE_MAX) return false;
    if (offset_dw < SETTINGS_OFFSET_MIN || offset_dw > SETTINGS_OFFSET_MAX)     return false;
    m_set.power_scale_x1000 = (uint16_t)scale_x1000;
    m_set.power_offset_dw   = (int16_t)offset_dw;
    changed();
    return true;
}

bool settings_set_wheel(int32_t mm)
{
    if (mm < SETTINGS_WHEEL_MIN || mm > SETTINGS_WHEEL_MAX) return false;
    m_set.wheel_mm = (uint16_t)mm;
    changed();
    return true;
}

bool settings_set_csc(int32_t mode)
{
    if (mode < 0 || mode > SETTINGS_CSC_MAX) return false;
    m_set.csc_mode = (uint8_t)mode;
    changed();
    return true;
}

void settings_reset(void)
{
    set_defaults();
    apply();
    m_src         = SRC_DEFAULT;
    m_save_pend   = false;
    m_delete_pend = true;
    kick();
}
