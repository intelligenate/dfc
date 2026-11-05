/*********************************************************************
 * DFC BLE Firmware — Peloton-to-BLE bridge
 *
 * Passively sniffs the Peloton bike -> head-unit serial responses via
 * the SP3222 RS-232 receiver (legacy UART, 19200 8N1) and broadcasts
 * them as BLE Cycling Power Service (CPS, with crank revolution data so
 * Apple Watch / Garmin derive cadence from the power meter) and Fitness
 * Machine Service (FTMS Indoor Bike Data: speed, cadence, resistance,
 * power).
 *
 * Peloton serial protocol (decoded by Imran Haque, PeloMon project,
 * https://ihaque.org/posts/2020/10/15/pelomon-part-i-decoding-peloton/):
 *   Head unit -> bike (jack TIP):  F5 <id> <cs> F6   (4 bytes, ~every 100 ms)
 *   Bike -> head unit (jack RING): F1 <id> <len> <ascii digits, LSB first> <cs> F6
 *   cs = sum of all preceding bytes, modulo 256.
 *   id 0x41 cadence (3 digits, rpm)      id 0x44 power (5 digits, deciwatts)
 *   id 0x4A resistance (4 digits, raw)   id 0xF7 resistance-table entry (4 digits)
 *   id 0xFB bike ID, id 0xFE init        (both only at head-unit boot)
 * At head-unit boot the HU reads 31 resistance-table entries (F7 00..1E)
 * that map raw resistance to the 0-100 % shown on screen.
 *
 * Transceiver control, settled on the bench 2026-10-12 by toggling each
 * signal live over BLE while the bike was running (see docs/peloton-protocol.md):
 *   P0.30 (3222_PWRDN): must be LOW. HIGH stops reception AND freezes the
 *                       tablet (RS-232 driver comes alive on the bus).
 *   P1.12 (3222_EN):    must be LOW to receive. HIGH = receiver output off.
 *                       Kept HIGH while no client is connected, like the original.
 *   TXD / RTS / CTS:    not connected to the UART. Pure listener.
 *
 * Target: nRF52840 (BMD-340-A-R)
 * SDK:    Nordic nRF5 SDK 16.0.0 + S140 v7.0.1 SoftDevice
 *
 * Copyright 2021 Intelligenate LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *********************************************************************/

/* APP_ERROR_CHECK in this file records err/line in the .noinit fault trace.
 * The SDK picks app_error_handler() (line + file) over app_error_handler_bare()
 * only when DEBUG is defined, so DEBUG is set for the one header that looks at
 * it. SDK-internal checks stay bare and show up in the trace as line 0. */
#define DEBUG 1
#include "app_error.h"
#undef DEBUG

#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "nordic_common.h"
#include "nrf.h"
#include "app_error.h"
#include "ble.h"
#include "ble_err.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_bas.h"
#include "ble_dis.h"
#include "ble_conn_params.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "app_timer.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"
#include "ble_conn_state.h"
#include "nrf_pwr_mgmt.h"
#include "nrf_delay.h"
#include "nrf_sdm.h"
#include "nrf_power.h"
#include "nrf_bootloader_info.h"
#include "ble_dfu.h"

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "ble_nus.h"
#include "peer_manager.h"
#include "settings.h"
#include <stdlib.h>
#include "peer_manager_handler.h"
#include "fds.h"

/*===================================================================
 * Defines
 *===================================================================*/

#define DEVICE_NAME                     "DFC"      /* same name as the stock firmware */
#define MANUFACTURER_NAME               "Intelligenate Industries"
#define MODEL_NUMBER                    "DFC"
#define FW_VERSION                      "0.2.0"
#define FIRMWARE_REVISION               FW_VERSION
#define HARDWARE_REVISION               "Rev 4"

/* BLE service UUIDs */
#define BLE_UUID_CPS                    0x1818
#define BLE_UUID_FTMS                   0x1826

/* BLE characteristic UUIDs */
#define BLE_UUID_CPS_MEASUREMENT        0x2A63
#define BLE_UUID_CPS_FEATURE            0x2A65
#define BLE_UUID_SENSOR_LOCATION        0x2A5D
#define BLE_UUID_CPS_CONTROL_POINT      0x2A66
/* Cycling Speed and Cadence (phase 5, csc mode 1/2) */
#define BLE_UUID_CSC                    0x1816
#define BLE_UUID_CSC_MEASUREMENT        0x2A5B
#define BLE_UUID_CSC_FEATURE            0x2A5C
#define APP_GAP_APPEARANCE_CSC          0x0485   /* Cycling: Speed and Cadence Sensor */
#define BLE_UUID_FTMS_INDOOR_BIKE       0x2AD2
#define BLE_UUID_FTMS_FEATURE           0x2ACC

/* Speed computation.
 * SPEED_FROM_POWER=1: speed derived from power with the piecewise
 *   regression PeloMon fitted against ~150 Peloton rides — this is how the
 *   Peloton head unit itself derives the speed it displays.
 * SPEED_FROM_POWER=0: legacy cadence * fixed gear estimate. */
#define SPEED_FROM_POWER                1
#define WHEEL_CIRCUMFERENCE             2.105f
#define GEAR_RATIO                      3.5f

/* Hardware pins */
#define LED_PIN                         28      /* P0.28, active HIGH */
#define UART_RX_PIN                     26      /* P0.26 <- SP3222 R1OUT */
#define UART_TX_PIN                     27      /* P0.27 -> SP3222 D1IN (never connected) */
#define UART_RTS_PIN                    5       /* P0.05, as the original (pca10056 default) */
#define UART_CTS_PIN                    7       /* P0.07, as the original (pca10456 default) */
#define SP3222_SHDN_PIN                 30      /* P0.30: original drives HIGH at init, always */
#define SP3222_EN_PIN                   12      /* P1.12: original LOW at idle, HIGH while serial open */
#define P0_25_PIN                       25      /* original: input with pull-up (SP3222 D2IN) */

/* LISTEN_ALWAYS=1: open the UART and enable the SP3222 receivers at boot
 * instead of on BLE connect. Electrically identical on the bus (the
 * driver is shut down either way); useful for bench diagnosis because SWD
 * register reads drop the BLE link and would otherwise close the UART.
 * 0 = original firmware behaviour (listen only while a client is connected). */
#ifndef LISTEN_ALWAYS
#define LISTEN_ALWAYS                   0
#endif

/* FTMS_ENABLED=0 builds a pure Cycling Power sensor (power + crank cadence),
 * like the original DFC firmware. Apple Watch appears to classify a device
 * that also exposes the Fitness Machine service as a "smart trainer" and
 * then ignores the power-meter data; Wahoo/Zwift use Cycling Power either way. */
#ifndef FTMS_ENABLED
#define FTMS_ENABLED                    1
#endif

/* app_timer tick rate (RTC1, prescaler APP_TIMER_CONFIG_RTC_FREQUENCY) */
#define TICKS_PER_SEC                   (APP_TIMER_CLOCK_FREQ / (APP_TIMER_CONFIG_RTC_FREQUENCY + 1))

/* BLE config */
#define APP_BLE_CONN_CFG_TAG            1
#define APP_BLE_OBSERVER_PRIO           3

/* Notification interval: 500ms */
#define NOTIFY_INTERVAL                 APP_TIMER_TICKS(500)

/* Connection parameters: match upstream power-cadence-beta.4 (PPCP read
 * from the shipped beta.4 binary). v1.5 used 500-1000 ms
 * / 4 s; beta.4 widened to 20-1000 ms / 10 s so Garmin's 1 s request (Edge
 * 130, Fenix 6X) and fast phone/watch intervals are both inside the range
 * and no renegotiation can fail (issues #2, #5). Update delays are not
 * recoverable from the binary; the developer stated beta.2 used a 1000 ms
 * first delay. */
#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(20, UNIT_1_25_MS)
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(1000, UNIT_1_25_MS)
#define SLAVE_LATENCY                   0
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(10000, UNIT_10_MS)

#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(1000)
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(5000)
#define MAX_CONN_PARAMS_UPDATE_COUNT    3

/* Advertising intervals (in units of 0.625 ms) */
#define APP_ADV_FAST_INTERVAL           32      /* 20 ms */
#define APP_ADV_SLOW_INTERVAL           244     /* 152.5 ms */
#define APP_ADV_FAST_DURATION           3000    /* 30 seconds (in 10ms units) */

/* GAP appearance: Cycling Power Sensor (0x0484). Apple Watch and Garmin
 * use this to classify the device as a cycling sensor. */
#define APP_GAP_APPEARANCE              0x0484

/* Peer Manager — Just-Works bonding, like the original firmware.
 * Apple Watch pairs "Health Devices" and expects to bond so it can
 * reconnect automatically at the start of every workout. */
#define SEC_PARAM_BOND                  1
#define SEC_PARAM_MITM                  0
#define SEC_PARAM_LESC                  0
#define SEC_PARAM_KEYPRESS              0
#define SEC_PARAM_IO_CAPABILITIES       BLE_GAP_IO_CAPS_NONE
#define SEC_PARAM_OOB                   0
#define SEC_PARAM_MIN_KEY_SIZE          7
#define SEC_PARAM_MAX_KEY_SIZE          16

/*===================================================================
 * Global instances
 *===================================================================*/

BLE_BAS_DEF(m_bas);
BLE_NUS_DEF(m_nus, NRF_SDH_BLE_TOTAL_LINK_COUNT);
NRF_BLE_GATT_DEF(m_gatt);
NRF_BLE_QWR_DEF(m_qwr);
BLE_ADVERTISING_DEF(m_advertising);
APP_TIMER_DEF(m_notify_timer_id);

/* Up to NRF_SDH_BLE_TOTAL_LINK_COUNT concurrent clients (e.g. watch + phone) */
static uint16_t m_conn[NRF_SDH_BLE_TOTAL_LINK_COUNT] =
{
    BLE_CONN_HANDLE_INVALID
#if NRF_SDH_BLE_TOTAL_LINK_COUNT > 1
    , BLE_CONN_HANDLE_INVALID
#endif
};

static uint8_t conn_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < NRF_SDH_BLE_TOTAL_LINK_COUNT; i++)
        if (m_conn[i] != BLE_CONN_HANDLE_INVALID) n++;
    return n;
}

static void conn_add(uint16_t h)
{
    for (uint8_t i = 0; i < NRF_SDH_BLE_TOTAL_LINK_COUNT; i++)
        if (m_conn[i] == BLE_CONN_HANDLE_INVALID) { m_conn[i] = h; return; }
}

static void conn_remove(uint16_t h)
{
    for (uint8_t i = 0; i < NRF_SDH_BLE_TOTAL_LINK_COUNT; i++)
        if (m_conn[i] == h) m_conn[i] = BLE_CONN_HANDLE_INVALID;
}

static void ble_log(const char * fmt, ...);
static void advertising_start(void);

/*===================================================================
 * Robustness (phase 4): no peer request may ever reach APP_ERROR_CHECK.
 * Every SoftDevice call made from an event handler goes through
 * sd_check(), which logs the error over NUS and carries on. A genuine
 * fault resets the board and leaves a record in .noinit RAM that the
 * status line reports (rst=<count> last=<id>:<err>@<line>). Upstream
 * beta.4 fixed the Garmin 16.13 reset loop (#19 #24 #25 #26) by
 * "ignoring the request"; this generalises that fix.
 *===================================================================*/

typedef struct
{
    uint32_t magic;
    uint32_t count;
    uint32_t id;
    uint32_t pc;     /* err_code for SDK errors, PC otherwise */
    uint32_t info;   /* line number for SDK errors/asserts, info otherwise */
} fault_rec_t;

#define FAULT_MAGIC 0x44464331UL   /* 'DFC1' */
static fault_rec_t m_fault __attribute__((section(".noinit")));
static uint32_t    m_sd_err;       /* SoftDevice errors ignored since boot */
static uint32_t m_ram_min;   /* SoftDevice\'s minimum app RAM start (status ram=) */

static void sd_check(const char * what, ret_code_t err)
{
    if (err == NRF_SUCCESS) return;
    m_sd_err++;
    ble_log("ERR %s 0x%lX", what, (unsigned long)err);
}

/* Call before the SoftDevice is enabled (NRF_POWER is restricted after). */
static void fault_record_init(void)
{
    uint32_t reas = NRF_POWER->RESETREAS;
    bool soft = (reas & (POWER_RESETREAS_SREQ_Msk | POWER_RESETREAS_LOCKUP_Msk |
                         POWER_RESETREAS_DOG_Msk)) != 0;
    if (!soft || m_fault.magic != FAULT_MAGIC)
    {
        memset(&m_fault, 0, sizeof(m_fault));
        m_fault.magic = FAULT_MAGIC;
    }
    NRF_POWER->RESETREAS = reas;   /* write-1-to-clear */
}

/*===================================================================
 * CPS GATTS handles (manual implementation)
 *===================================================================*/

static uint16_t                 m_cps_service_handle;
static ble_gatts_char_handles_t m_cps_power_meas_handles;
static ble_gatts_char_handles_t m_cps_feature_handles;
static ble_gatts_char_handles_t m_cps_sensor_loc_handles;
static ble_gatts_char_handles_t m_cps_cp_handles;

/* CSC service (created when csc mode != 0; CPS is skipped in mode 1) */
static uint16_t                 m_csc_service_handle;
static ble_gatts_char_handles_t m_csc_meas_handles;
static ble_gatts_char_handles_t m_csc_feature_handles;
static ble_gatts_char_handles_t m_csc_loc_handles;
static uint8_t                  m_csc_mode;   /* settings csc_mode, read once at boot */

/*===================================================================
 * FTMS GATTS handles (manual implementation)
 *===================================================================*/

#if FTMS_ENABLED
static uint16_t                 m_ftms_service_handle;
static ble_gatts_char_handles_t m_ftms_indoor_bike_handles;
static ble_gatts_char_handles_t m_ftms_feature_handles;
#endif

/*===================================================================
 * Peloton protocol decoding + ride state live in peloton.c (platform
 * independent, unit-tested on the host: see tests/test_peloton.c).
 *===================================================================*/

#include "peloton.h"

static void nus_data_handler(ble_nus_evt_t * p_evt);
static void cps_control_point_on_write(uint16_t conn_handle, ble_gatts_evt_write_t const * p_write);

/*===================================================================
 * UART via nrfx_uart — legacy UART (ENABLE=4), IRQ mode, one byte per
 * RXDRDY, exactly like the original firmware's register state:
 *   ENABLE=4, BAUDRATE=19200, RXD=P0.26, IRQ priority 6
 * Only RXD is connected. TXD stays disconnected and the SP3222 driver is
 * held in shutdown (P0.30 LOW), so this board can never drive the bus.
 * HWFC is off: the pca10056 RTS/CTS pins are not wired on the DFC.
 *===================================================================*/

#include "nrfx_uart.h"

/* Runtime-adjustable options (via NUS commands, see nus_data_handler) */
static bool m_cfg_hwfc = false;   /* hardware flow control, RTS P0.05 / CTS P0.07 */
static bool m_cfg_txd  = false;   /* connect TXD P0.27 to the UART (idle high) */
static bool m_cfg_en_on_open = true;  /* drive P1.12 LOW when the UART opens, HIGH on close */

/* Raw byte capture for protocol debugging ('d' command) */
#define RAW_DUMP_LEN 96
static volatile uint8_t  m_raw[RAW_DUMP_LEN];
static volatile uint16_t m_raw_n   = RAW_DUMP_LEN;   /* == LEN means idle */
static volatile bool     m_raw_ready = false;

static volatile bool     m_uart_enabled  = false;
static volatile uint32_t m_uart_rx_count = 0;
static volatile uint32_t m_uart_err_count = 0;

static uint8_t m_rx_byte;
static const nrfx_uart_t m_uart = NRFX_UART_INSTANCE(0);

static void uart_event_handler(nrfx_uart_event_t const * p_event, void * p_context)
{
    switch (p_event->type)
    {
        case NRFX_UART_EVT_RX_DONE:
            if (p_event->data.rxtx.bytes > 0)
            {
                peloton_parse_byte(m_rx_byte, app_timer_cnt_get());
                m_uart_rx_count++;
                if (m_raw_n < RAW_DUMP_LEN)
                {
                    m_raw[m_raw_n++] = m_rx_byte;
                    if (m_raw_n == RAW_DUMP_LEN) m_raw_ready = true;
                }
            }
            nrfx_uart_rx(&m_uart, &m_rx_byte, 1);
            break;

        case NRFX_UART_EVT_ERROR:
            /* Overrun/framing: resync parser and keep receiving */
            m_uart_err_count++;
            peloton_resync();
            nrfx_uart_rx(&m_uart, &m_rx_byte, 1);
            break;

        default:
            break;
    }
}

static void sp3222_receivers_enable(bool enable)
{
    /* P1.12 LOW = receiver output active (bench-verified 2026-10-12) */
    if (enable) NRF_P1->OUTCLR = (1UL << SP3222_EN_PIN);
    else        NRF_P1->OUTSET = (1UL << SP3222_EN_PIN);
}

static void uart_open(void)
{
    if (m_uart_enabled) return;

    /* Identical to the original firmware's nrf_serial configuration */
    nrfx_uart_config_t config = NRFX_UART_DEFAULT_CONFIG;
    config.pselrxd  = UART_RX_PIN;
    config.pseltxd  = m_cfg_txd  ? UART_TX_PIN  : NRF_UART_PSEL_DISCONNECTED;
    config.pselrts  = m_cfg_hwfc ? UART_RTS_PIN : NRF_UART_PSEL_DISCONNECTED;
    config.pselcts  = m_cfg_hwfc ? UART_CTS_PIN : NRF_UART_PSEL_DISCONNECTED;
    config.hwfc     = m_cfg_hwfc ? NRF_UART_HWFC_ENABLED : NRF_UART_HWFC_DISABLED;
    config.parity   = NRF_UART_PARITY_EXCLUDED;
    config.baudrate = NRF_UART_BAUDRATE_19200;
    config.interrupt_priority = 6;

    ret_code_t err_code = nrfx_uart_init(&m_uart, &config, uart_event_handler);
    APP_ERROR_CHECK(err_code);

    /* Route the IRQ through the SoftDevice NVIC API (equivalent to the
     * driver's own NVIC_EnableIRQ for a non-reserved IRQ, kept explicit). */
    sd_nvic_SetPriority(UARTE0_UART0_IRQn, 6);
    sd_nvic_ClearPendingIRQ(UARTE0_UART0_IRQn);
    sd_nvic_EnableIRQ(UARTE0_UART0_IRQn);

    peloton_resync();
    nrfx_uart_rx_enable(&m_uart);
    err_code = nrfx_uart_rx(&m_uart, &m_rx_byte, 1);
    APP_ERROR_CHECK(err_code);

    if (m_cfg_en_on_open) sp3222_receivers_enable(true);   /* P1.12 LOW: receiver on */

    m_uart_enabled  = true;
    m_uart_rx_count = 0;
    m_uart_err_count = 0;

    NRF_LOG_INFO("UART open: legacy 19200 8N1, RX P0.26 only, P1.12 low");
}

static void uart_close(void)
{
    if (!m_uart_enabled) return;

    if (m_cfg_en_on_open) sp3222_receivers_enable(false);   /* P1.12 HIGH: receiver off while idle */

    nrfx_uart_rx_abort(&m_uart);
    nrfx_uart_uninit(&m_uart);

    /* Return the serial pins to floating inputs */
    NRF_P0->PIN_CNF[UART_TX_PIN]  = 0x02;
    NRF_P0->PIN_CNF[UART_RTS_PIN] = 0x02;
    NRF_P0->PIN_CNF[UART_CTS_PIN] = 0x02;

    m_uart_enabled = false;

    NRF_LOG_INFO("UART closed. rx=%d err=%d", m_uart_rx_count, m_uart_err_count);
}

/*===================================================================
 * Logging init
 *===================================================================*/

static void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);
    NRF_LOG_DEFAULT_BACKENDS_INIT();
}

/*===================================================================
 * Timer init
 *===================================================================*/

static void notify_timer_handler(void * p_context);

static void timers_init(void)
{
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&m_notify_timer_id,
                                APP_TIMER_MODE_REPEATED,
                                notify_timer_handler);
    APP_ERROR_CHECK(err_code);
}

static void timers_start(void)
{
    ret_code_t err_code = app_timer_start(m_notify_timer_id, NOTIFY_INTERVAL, NULL);
    APP_ERROR_CHECK(err_code);
}

/*===================================================================
 * Power management
 *===================================================================*/

static void power_management_init(void)
{
    ret_code_t err_code = nrf_pwr_mgmt_init();
    APP_ERROR_CHECK(err_code);
}

/*===================================================================
 * BLE event handler
 *===================================================================*/

static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    uint16_t const h = p_ble_evt->evt.gap_evt.conn_handle;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
        {
            ble_gap_conn_params_t const * cp = &p_ble_evt->evt.gap_evt.params.connected.conn_params;
            conn_add(h);
            sd_check("qwr_assign", nrf_ble_qwr_conn_handle_assign(&m_qwr, h));
            if (!LISTEN_ALWAYS && conn_count() == 1) uart_open();
            ble_log("EVT conn h=%u n=%u %02X:%02X:%02X int=%u lat=%u to=%u", h, conn_count(),
                    p_ble_evt->evt.gap_evt.params.connected.peer_addr.addr[5],
                    p_ble_evt->evt.gap_evt.params.connected.peer_addr.addr[4],
                    p_ble_evt->evt.gap_evt.params.connected.peer_addr.addr[3],
                    cp->max_conn_interval, cp->slave_latency, cp->conn_sup_timeout);
            /* keep a slot open for a second client */
            if (conn_count() < NRF_SDH_BLE_TOTAL_LINK_COUNT) advertising_start();
            break;
        }

        case BLE_GAP_EVT_DISCONNECTED:
            conn_remove(h);
            ble_log("EVT disc h=%u reason=0x%02X n=%u", h,
                    p_ble_evt->evt.gap_evt.params.disconnected.reason, conn_count());
            if (!LISTEN_ALWAYS && conn_count() == 0) uart_close();
            break;

        case BLE_GAP_EVT_CONN_PARAM_UPDATE:
            ble_log("EVT cparam h=%u int=%u lat=%u to=%u", h,
                    p_ble_evt->evt.gap_evt.params.conn_param_update.conn_params.max_conn_interval,
                    p_ble_evt->evt.gap_evt.params.conn_param_update.conn_params.slave_latency,
                    p_ble_evt->evt.gap_evt.params.conn_param_update.conn_params.conn_sup_timeout);
            break;

        case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST:
            /* Central-role event; cannot occur for a peripheral. Logged so an
             * unexpected Garmin request becomes visible instead of fatal. */
            ble_log("EVT cparam-req h=%u (ignored)", h);
            break;

        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
        {
            ble_gap_phys_t const phys = { .rx_phys = BLE_GAP_PHY_AUTO, .tx_phys = BLE_GAP_PHY_AUTO };
            ble_log("EVT phy-req h=%u", h);
            sd_check("phy_update", sd_ble_gap_phy_update(h, &phys));
            break;
        }

        case BLE_GAP_EVT_PHY_UPDATE:
            ble_log("EVT phy h=%u st=%u tx=%u rx=%u", h,
                    p_ble_evt->evt.gap_evt.params.phy_update.status,
                    p_ble_evt->evt.gap_evt.params.phy_update.tx_phy,
                    p_ble_evt->evt.gap_evt.params.phy_update.rx_phy);
            break;

        /* Data length and MTU requests are answered by nrf_ble_gatt
         * (NRF_BLE_GATT_ENABLED). Never reply a second time: the
         * SoftDevice returns INVALID_STATE for a duplicate reply. */
        case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST:
            ble_log("EVT dlu-req h=%u", h);
            break;
        case BLE_GAP_EVT_DATA_LENGTH_UPDATE:
            ble_log("EVT dlu h=%u tx=%u rx=%u", h,
                    p_ble_evt->evt.gap_evt.params.data_length_update.effective_params.max_tx_octets,
                    p_ble_evt->evt.gap_evt.params.data_length_update.effective_params.max_rx_octets);
            break;
        case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST:
            ble_log("EVT mtu-req h=%u mtu=%u", p_ble_evt->evt.gatts_evt.conn_handle,
                    p_ble_evt->evt.gatts_evt.params.exchange_mtu_request.client_rx_mtu);
            break;

        /* Pairing: the Peer Manager (higher-priority observer) replies to
         * SEC_PARAMS_REQUEST / SEC_INFO_REQUEST and consumes AUTH_STATUS,
         * CONN_SEC_UPDATE and SYS_ATTR_MISSING. Only trace them here. */
        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            ble_log("EVT sec-params-req h=%u bond=%u mitm=%u lesc=%u io=%u", h,
                    p_ble_evt->evt.gap_evt.params.sec_params_request.peer_params.bond,
                    p_ble_evt->evt.gap_evt.params.sec_params_request.peer_params.mitm,
                    p_ble_evt->evt.gap_evt.params.sec_params_request.peer_params.lesc,
                    p_ble_evt->evt.gap_evt.params.sec_params_request.peer_params.io_caps);
            break;
        case BLE_GAP_EVT_SEC_INFO_REQUEST:
            ble_log("EVT sec-info-req h=%u", h);
            break;
        case BLE_GAP_EVT_AUTH_STATUS:
            ble_log("EVT auth h=%u st=0x%02X bonded=%u", h,
                    p_ble_evt->evt.gap_evt.params.auth_status.auth_status,
                    p_ble_evt->evt.gap_evt.params.auth_status.bonded);
            break;
        case BLE_GAP_EVT_CONN_SEC_UPDATE:
            ble_log("EVT sec-upd h=%u sm=%u lv=%u", h,
                    p_ble_evt->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode.sm,
                    p_ble_evt->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode.lv);
            break;
        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            ble_log("EVT sysattr h=%u", p_ble_evt->evt.gatts_evt.conn_handle);
            break;

        /* Requests we cannot satisfy (no I/O capabilities, LESC disabled):
         * answer so the pairing fails cleanly instead of hanging. */
        case BLE_GAP_EVT_AUTH_KEY_REQUEST:
            ble_log("EVT authkey-req h=%u type=%u", h,
                    p_ble_evt->evt.gap_evt.params.auth_key_request.key_type);
            sd_check("auth_key_reply", sd_ble_gap_auth_key_reply(h, BLE_GAP_AUTH_KEY_TYPE_NONE, NULL));
            break;
        case BLE_GAP_EVT_LESC_DHKEY_REQUEST:
            ble_log("EVT lesc-req h=%u (unsupported, disconnecting)", h);
            sd_check("lesc_disc", sd_ble_gap_disconnect(h, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION));
            break;
        case BLE_GAP_EVT_PASSKEY_DISPLAY:
        case BLE_GAP_EVT_KEY_PRESSED:
            ble_log("EVT passkey h=%u (ignored)", h);
            break;

        case BLE_GATTS_EVT_WRITE:
        {
            ble_gatts_evt_write_t const * w = &p_ble_evt->evt.gatts_evt.params.write;
            uint16_t hw = p_ble_evt->evt.gatts_evt.conn_handle;
            if (w->handle == m_cps_power_meas_handles.cccd_handle)
                ble_log("EVT cccd CPS h=%u val=%u", hw, w->len ? w->data[0] : 0);
#if FTMS_ENABLED
            else if (w->handle == m_ftms_indoor_bike_handles.cccd_handle)
                ble_log("EVT cccd FTMS h=%u val=%u", hw, w->len ? w->data[0] : 0);
#endif
            else if (w->handle == m_cps_cp_handles.cccd_handle)
                ble_log("EVT cccd CP h=%u val=%u", hw, w->len ? w->data[0] : 0);
            else if (m_csc_mode != 0 && w->handle == m_csc_meas_handles.cccd_handle)
                ble_log("EVT cccd CSC h=%u val=%u", hw, w->len ? w->data[0] : 0);
            else if (w->handle == m_cps_cp_handles.value_handle)
                ble_log("EVT cp write h=%u op=0x%02X", hw, w->len ? w->data[0] : 0);
            cps_control_point_on_write(hw, w);
            break;
        }

        case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST:
            /* answered by nrf_ble_qwr */
            ble_log("EVT rw-auth h=%u", p_ble_evt->evt.gatts_evt.conn_handle);
            break;

        case BLE_GATTS_EVT_HVC:
        case BLE_GATTS_EVT_HVN_TX_COMPLETE:
            break;

        case BLE_GAP_EVT_TIMEOUT:
            ble_log("EVT timeout src=%u", p_ble_evt->evt.gap_evt.params.timeout.src);
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            ble_log("EVT gattc-timeout h=%u", p_ble_evt->evt.gattc_evt.conn_handle);
            sd_check("gattc_to_disc", sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION));
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            ble_log("EVT gatts-timeout h=%u", p_ble_evt->evt.gatts_evt.conn_handle);
            sd_check("gatts_to_disc", sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION));
            break;

        default:
            /* The "ignore the request" behaviour upstream beta.4 shipped:
             * anything we do not understand is traced, never fatal. */
            ble_log("EVT ? id=0x%X", p_ble_evt->header.evt_id);
            break;
    }
}

/*===================================================================
 * BLE stack init
 *===================================================================*/

static void ble_stack_init(void)
{
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    /* Configure the BLE stack using the default settings from sdk_config.h.
     * Fetch the start address of the application RAM. */
    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    /* Enable BLE stack */
    err_code = nrf_sdh_ble_enable(&ram_start);
    m_ram_min = ram_start;   /* SD writes back its minimum; linker RAM origin must be >= this */
    APP_ERROR_CHECK(err_code);

    /* Register a handler for BLE events */
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
}

/*===================================================================
 * GAP parameters
 *===================================================================*/

static void gap_params_init(void)
{
    ret_code_t              err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *)DEVICE_NAME,
                                          strlen(DEVICE_NAME));
    APP_ERROR_CHECK(err_code);

    err_code = sd_ble_gap_appearance_set(m_csc_mode == 1 ? APP_GAP_APPEARANCE_CSC : APP_GAP_APPEARANCE);
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));
    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}

/*===================================================================
 * GATT init
 *===================================================================*/

static void gatt_init(void)
{
    ret_code_t err_code = nrf_ble_gatt_init(&m_gatt, NULL);
    APP_ERROR_CHECK(err_code);
}

/*===================================================================
 * QWR error handler
 *===================================================================*/

static void nrf_qwr_error_handler(uint32_t nrf_error)
{
    NRF_LOG_WARNING("QWR error: 0x%x — ignoring.", nrf_error);
}

/*===================================================================
 * CPS service init (manual GATTS)
 *===================================================================*/

static void cps_init(void)
{
    ret_code_t    err_code;
    ble_uuid_t    service_uuid;
    ble_uuid_t    char_uuid;

    /* Add CPS service */
    BLE_UUID_BLE_ASSIGN(service_uuid, BLE_UUID_CPS);
    err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                        &service_uuid,
                                        &m_cps_service_handle);
    APP_ERROR_CHECK(err_code);

    /* --- Power Measurement characteristic (0x2A63) — Notify --- */
    {
        ble_gatts_char_md_t char_md;
        ble_gatts_attr_md_t cccd_md;
        ble_gatts_attr_t    attr_char_value;
        ble_gatts_attr_md_t attr_md;

        memset(&cccd_md, 0, sizeof(cccd_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
        cccd_md.vloc = BLE_GATTS_VLOC_STACK;

        memset(&char_md, 0, sizeof(char_md));
        char_md.char_props.notify = 1;
        char_md.p_cccd_md         = &cccd_md;

        BLE_UUID_BLE_ASSIGN(char_uuid, BLE_UUID_CPS_MEASUREMENT);

        memset(&attr_md, 0, sizeof(attr_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
        attr_md.vloc    = BLE_GATTS_VLOC_STACK;
        attr_md.vlen    = 1;  /* Variable length */

        uint8_t init_value[8] = {0};

        memset(&attr_char_value, 0, sizeof(attr_char_value));
        attr_char_value.p_uuid    = &char_uuid;
        attr_char_value.p_attr_md = &attr_md;
        attr_char_value.init_len  = 4;
        attr_char_value.init_offs = 0;
        attr_char_value.max_len   = 20;
        attr_char_value.p_value   = init_value;

        err_code = sd_ble_gatts_characteristic_add(m_cps_service_handle,
                                                   &char_md,
                                                   &attr_char_value,
                                                   &m_cps_power_meas_handles);
        APP_ERROR_CHECK(err_code);
    }

    /* --- Power Feature characteristic (0x2A65) — Read --- */
    {
        ble_gatts_char_md_t char_md;
        ble_gatts_attr_t    attr_char_value;
        ble_gatts_attr_md_t attr_md;

        memset(&char_md, 0, sizeof(char_md));
        char_md.char_props.read = 1;

        BLE_UUID_BLE_ASSIGN(char_uuid, BLE_UUID_CPS_FEATURE);

        memset(&attr_md, 0, sizeof(attr_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
        attr_md.vloc = BLE_GATTS_VLOC_STACK;

        /* Cycling Power Feature: bit 2 = Wheel Revolution Data Supported,
         * bit 3 = Crank Revolution Data Supported. Crank data gives clients
         * cadence; wheel data gives Apple Watch (and others) speed and distance,
         * the way a PowerTap hub does. */
        uint8_t feature_value[4] = {0x0C, 0x00, 0x00, 0x00};

        memset(&attr_char_value, 0, sizeof(attr_char_value));
        attr_char_value.p_uuid    = &char_uuid;
        attr_char_value.p_attr_md = &attr_md;
        attr_char_value.init_len  = 4;
        attr_char_value.init_offs = 0;
        attr_char_value.max_len   = 4;
        attr_char_value.p_value   = feature_value;

        err_code = sd_ble_gatts_characteristic_add(m_cps_service_handle,
                                                   &char_md,
                                                   &attr_char_value,
                                                   &m_cps_feature_handles);
        APP_ERROR_CHECK(err_code);
    }

    /* --- Sensor Location characteristic (0x2A5D) — Read --- */
    {
        ble_gatts_char_md_t char_md;
        ble_gatts_attr_t    attr_char_value;
        ble_gatts_attr_md_t attr_md;

        memset(&char_md, 0, sizeof(char_md));
        char_md.char_props.read = 1;

        BLE_UUID_BLE_ASSIGN(char_uuid, BLE_UUID_SENSOR_LOCATION);

        memset(&attr_md, 0, sizeof(attr_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
        attr_md.vloc = BLE_GATTS_VLOC_STACK;

        uint8_t sensor_loc = 5;  /* Left Crank */

        memset(&attr_char_value, 0, sizeof(attr_char_value));
        attr_char_value.p_uuid    = &char_uuid;
        attr_char_value.p_attr_md = &attr_md;
        attr_char_value.init_len  = 1;
        attr_char_value.init_offs = 0;
        attr_char_value.max_len   = 1;
        attr_char_value.p_value   = &sensor_loc;

        err_code = sd_ble_gatts_characteristic_add(m_cps_service_handle,
                                                   &char_md,
                                                   &attr_char_value,
                                                   &m_cps_sensor_loc_handles);
        APP_ERROR_CHECK(err_code);
    }

    /* --- Cycling Power Control Point (0x2A66) — Write + Indicate ---
     * Real power meters all expose this; we answer every op code with
     * "Op Code not supported" so clients that probe it (Apple Watch's
     * calibration menu, Garmin) get a well-formed reply. */
    {
        ble_gatts_char_md_t char_md;
        ble_gatts_attr_md_t cccd_md;
        ble_gatts_attr_t    attr_char_value;
        ble_gatts_attr_md_t attr_md;

        memset(&cccd_md, 0, sizeof(cccd_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
        cccd_md.vloc = BLE_GATTS_VLOC_STACK;

        memset(&char_md, 0, sizeof(char_md));
        char_md.char_props.write    = 1;
        char_md.char_props.indicate = 1;
        char_md.p_cccd_md           = &cccd_md;

        BLE_UUID_BLE_ASSIGN(char_uuid, BLE_UUID_CPS_CONTROL_POINT);

        memset(&attr_md, 0, sizeof(attr_md));
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
        attr_md.vloc = BLE_GATTS_VLOC_STACK;
        attr_md.vlen = 1;

        uint8_t init_value[1] = {0};

        memset(&attr_char_value, 0, sizeof(attr_char_value));
        attr_char_value.p_uuid    = &char_uuid;
        attr_char_value.p_attr_md = &attr_md;
        attr_char_value.init_len  = 0;
        attr_char_value.init_offs = 0;
        attr_char_value.max_len   = 20;
        attr_char_value.p_value   = init_value;

        err_code = sd_ble_gatts_characteristic_add(m_cps_service_handle,
                                                   &char_md,
                                                   &attr_char_value,
                                                   &m_cps_cp_handles);
        APP_ERROR_CHECK(err_code);
    }
}

/* Reply to a Cycling Power Control Point write with an indication:
 * Response Code (0x20), request op code, result 0x02 = not supported. */
static void cps_control_point_on_write(uint16_t conn_handle, ble_gatts_evt_write_t const * p_write)
{
    if (p_write->handle != m_cps_cp_handles.value_handle || p_write->len < 1) return;

    uint8_t  rsp[3] = {0x20, p_write->data[0], 0x02};
    uint16_t len    = sizeof(rsp);
    ble_gatts_hvx_params_t hvx;
    memset(&hvx, 0, sizeof(hvx));
    hvx.handle = m_cps_cp_handles.value_handle;
    hvx.type   = BLE_GATT_HVX_INDICATION;
    hvx.p_len  = &len;
    hvx.p_data = rsp;
    (void)sd_ble_gatts_hvx(conn_handle, &hvx);
}

/*===================================================================
 * FTMS service init (manual GATTS)
 *===================================================================*/

#if FTMS_ENABLED
static void ftms_init(void)
{
    ret_code_t    err_code;
    ble_uuid_t    service_uuid;
    ble_uuid_t    char_uuid;

    /* Add FTMS service */
    BLE_UUID_BLE_ASSIGN(service_uuid, BLE_UUID_FTMS);
    err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,
                                        &service_uuid,
                                        &m_ftms_service_handle);
    APP_ERROR_CHECK(err_code);

    /* --- Indoor Bike Data characteristic (0x2AD2) — Notify --- */
    {
        ble_gatts_char_md_t char_md;
        ble_gatts_attr_md_t cccd_md;
        ble_gatts_attr_t    attr_char_value;
        ble_gatts_attr_md_t attr_md;

        memset(&cccd_md, 0, sizeof(cccd_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
        cccd_md.vloc = BLE_GATTS_VLOC_STACK;

        memset(&char_md, 0, sizeof(char_md));
        char_md.char_props.notify = 1;
        char_md.p_cccd_md         = &cccd_md;

        BLE_UUID_BLE_ASSIGN(char_uuid, BLE_UUID_FTMS_INDOOR_BIKE);

        memset(&attr_md, 0, sizeof(attr_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
        attr_md.vloc = BLE_GATTS_VLOC_STACK;

        uint8_t init_value[10] = {0};

        memset(&attr_char_value, 0, sizeof(attr_char_value));
        attr_char_value.p_uuid    = &char_uuid;
        attr_char_value.p_attr_md = &attr_md;
        attr_char_value.init_len  = 10;
        attr_char_value.init_offs = 0;
        attr_char_value.max_len   = 10;
        attr_char_value.p_value   = init_value;

        err_code = sd_ble_gatts_characteristic_add(m_ftms_service_handle,
                                                   &char_md,
                                                   &attr_char_value,
                                                   &m_ftms_indoor_bike_handles);
        APP_ERROR_CHECK(err_code);
    }

    /* --- Fitness Machine Feature characteristic (0x2ACC) — Read --- */
    {
        ble_gatts_char_md_t char_md;
        ble_gatts_attr_t    attr_char_value;
        ble_gatts_attr_md_t attr_md;

        memset(&char_md, 0, sizeof(char_md));
        char_md.char_props.read = 1;

        BLE_UUID_BLE_ASSIGN(char_uuid, BLE_UUID_FTMS_FEATURE);

        memset(&attr_md, 0, sizeof(attr_md));
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
        attr_md.vloc = BLE_GATTS_VLOC_STACK;

        /* Machine Features: 0x00004082 (cadence bit1 + resistance bit7 + power bit14)
         * Target Settings:  0x00000000
         * Stored little-endian. */
        uint8_t feature_value[8] = {0x82, 0x40, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00};

        memset(&attr_char_value, 0, sizeof(attr_char_value));
        attr_char_value.p_uuid    = &char_uuid;
        attr_char_value.p_attr_md = &attr_md;
        attr_char_value.init_len  = 8;
        attr_char_value.init_offs = 0;
        attr_char_value.max_len   = 8;
        attr_char_value.p_value   = feature_value;

        err_code = sd_ble_gatts_characteristic_add(m_ftms_service_handle,
                                                   &char_md,
                                                   &attr_char_value,
                                                   &m_ftms_feature_handles);
        APP_ERROR_CHECK(err_code);
    }
}

#endif /* FTMS_ENABLED */

/*===================================================================
 * Services init
 *===================================================================*/

/*===================================================================
 * Buttonless DFU (Nordic Secure DFU, service FE59)
 *
 * The stock DFC app has this service; without it a unit updated to this
 * firmware could never be updated again. ble_dfu_buttonless_async_svci_init()
 * needs a bootloader on the chip: on a board flashed without one it fails
 * and the status line shows dfu=0.
 *===================================================================*/

static bool m_dfu_ready;

static bool app_shutdown_handler(nrf_pwr_mgmt_evt_t event)
{
    switch (event)
    {
        case NRF_PWR_MGMT_EVT_PREPARE_DFU:
            /* wait until every client is gone (the SDK retries every
             * NRF_PWR_MGMT_CONFIG_AUTO_SHUTDOWN_RETRY tick) */
            if (conn_count() != 0) return false;
            ble_log("EVT dfu enter");
            return true;
        default:
            return true;
    }
}

NRF_PWR_MGMT_HANDLER_REGISTER(app_shutdown_handler, 0);

static void ble_dfu_evt_handler(ble_dfu_buttonless_evt_type_t event)
{
    switch (event)
    {
        case BLE_DFU_EVT_BOOTLOADER_ENTER_PREPARE:
            ble_log("EVT dfu prepare");
            sd_check("adv_stop", sd_ble_gap_adv_stop(m_advertising.adv_handle));
            for (uint8_t i = 0; i < NRF_SDH_BLE_TOTAL_LINK_COUNT; i++)
            {
                if (m_conn[i] == BLE_CONN_HANDLE_INVALID) continue;
                sd_check("dfu_disc", sd_ble_gap_disconnect(m_conn[i],
                                                           BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION));
            }
            break;
        case BLE_DFU_EVT_BOOTLOADER_ENTER:
            /* reset into the bootloader follows */
            break;
        case BLE_DFU_EVT_BOOTLOADER_ENTER_FAILED:
        case BLE_DFU_EVT_RESPONSE_SEND_ERROR:
            ble_log("EVT dfu FAIL %u", (unsigned)event);
            advertising_start();
            break;
        default:
            break;
    }
}

/*===================================================================
 * CSC service (manual GATTS) — Cycling Speed and Cadence, for clients
 * that want a speed/cadence sensor rather than a power meter (Garmin
 * Venu 3, upstream #15). Wheel + crank data mirror the CPS packet; the
 * wheel event time is 1/1024 s here (CPS: 1/2048 s).
 *===================================================================*/

static void csc_char_add(uint16_t uuid, bool notify, const uint8_t * value, uint16_t len,
                         uint16_t max_len, ble_gatts_char_handles_t * handles)
{
    ble_gatts_char_md_t char_md;
    ble_gatts_attr_md_t cccd_md;
    ble_gatts_attr_t    attr_char_value;
    ble_gatts_attr_md_t attr_md;
    ble_uuid_t          char_uuid;

    memset(&cccd_md, 0, sizeof(cccd_md));
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
    cccd_md.vloc = BLE_GATTS_VLOC_STACK;

    memset(&char_md, 0, sizeof(char_md));
    char_md.char_props.notify = notify ? 1 : 0;
    char_md.char_props.read   = notify ? 0 : 1;
    char_md.p_cccd_md         = notify ? &cccd_md : NULL;

    BLE_UUID_BLE_ASSIGN(char_uuid, uuid);

    memset(&attr_md, 0, sizeof(attr_md));
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
    attr_md.vloc = BLE_GATTS_VLOC_STACK;
    attr_md.vlen = (max_len != len) ? 1 : 0;

    memset(&attr_char_value, 0, sizeof(attr_char_value));
    attr_char_value.p_uuid    = &char_uuid;
    attr_char_value.p_attr_md = &attr_md;
    attr_char_value.init_len  = len;
    attr_char_value.init_offs = 0;
    attr_char_value.max_len   = max_len;
    attr_char_value.p_value   = (uint8_t *)value;

    ret_code_t err_code = sd_ble_gatts_characteristic_add(m_csc_service_handle, &char_md,
                                                          &attr_char_value, handles);
    APP_ERROR_CHECK(err_code);
}

static void csc_init(void)
{
    ble_uuid_t service_uuid;
    BLE_UUID_BLE_ASSIGN(service_uuid, BLE_UUID_CSC);
    ret_code_t err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &service_uuid,
                                                   &m_csc_service_handle);
    APP_ERROR_CHECK(err_code);

    /* CSC Measurement (0x2A5B), notify: flags(1) wheel revs(4) wheel time(2)
     * crank revs(2) crank time(2) = 11 bytes, flags 0x03 = both present. */
    static const uint8_t meas_init[11] = {0x03};
    csc_char_add(BLE_UUID_CSC_MEASUREMENT, true, meas_init, sizeof(meas_init), sizeof(meas_init), &m_csc_meas_handles);

    /* CSC Feature (0x2A5C): bit 0 wheel revolution data, bit 1 crank revolution data */
    static const uint8_t feature[2] = {0x03, 0x00};
    csc_char_add(BLE_UUID_CSC_FEATURE, false, feature, sizeof(feature), sizeof(feature), &m_csc_feature_handles);

    /* Sensor Location (0x2A5D): 12 = rear wheel */
    static const uint8_t loc = 12;
    csc_char_add(BLE_UUID_SENSOR_LOCATION, false, &loc, 1, 1, &m_csc_loc_handles);
}

static void services_init(void)
{
    ret_code_t         err_code;
    nrf_ble_qwr_init_t qwr_init = {0};
    ble_bas_init_t     bas_init;
    ble_dis_init_t     dis_init;

    /* QWR (Queued Write Request module) */
    qwr_init.error_handler = nrf_qwr_error_handler;
    err_code = nrf_ble_qwr_init(&m_qwr, &qwr_init);
    APP_ERROR_CHECK(err_code);

    /* Battery Service */
    memset(&bas_init, 0, sizeof(bas_init));
    bas_init.evt_handler          = NULL;
    bas_init.support_notification = true;
    bas_init.p_report_ref         = NULL;
    bas_init.initial_batt_level   = 100;
    bas_init.bl_rd_sec            = SEC_OPEN;
    bas_init.bl_cccd_wr_sec       = SEC_OPEN;
    bas_init.bl_report_rd_sec     = SEC_OPEN;

    err_code = ble_bas_init(&m_bas, &bas_init);
    APP_ERROR_CHECK(err_code);

    /* Device Information Service */
    memset(&dis_init, 0, sizeof(dis_init));
    ble_srv_ascii_to_utf8(&dis_init.manufact_name_str, (char *)MANUFACTURER_NAME);
    ble_srv_ascii_to_utf8(&dis_init.model_num_str, (char *)MODEL_NUMBER);
    ble_srv_ascii_to_utf8(&dis_init.fw_rev_str, (char *)FIRMWARE_REVISION);
    ble_srv_ascii_to_utf8(&dis_init.hw_rev_str, (char *)HARDWARE_REVISION);

    dis_init.dis_char_rd_sec = SEC_OPEN;

    err_code = ble_dis_init(&dis_init);
    APP_ERROR_CHECK(err_code);

    /* Cycling Power Service (manual) — not in csc mode 1 (CSC only) */
    if (m_csc_mode != 1) cps_init();

    /* Cycling Speed and Cadence — csc mode 1 (only) or 2 (with CPS) */
    if (m_csc_mode != 0) csc_init();

#if FTMS_ENABLED
    /* Fitness Machine Service (manual) */
    ftms_init();
#endif

    /* Nordic UART Service (NUS) — BLE logging */
    {
        ble_nus_init_t nus_init;
        memset(&nus_init, 0, sizeof(nus_init));
        nus_init.data_handler = nus_data_handler;  /* live pin/UART tuning commands */
        err_code = ble_nus_init(&m_nus, &nus_init);
        APP_ERROR_CHECK(err_code);
    }

    /* Buttonless DFU (unbonded DfuTarg flow, like the stock firmware) */
    {
        ble_dfu_buttonless_init_t dfus_init = {0};
        dfus_init.evt_handler = ble_dfu_evt_handler;
        err_code = ble_dfu_buttonless_init(&dfus_init);
        APP_ERROR_CHECK(err_code);
    }
}

/*===================================================================
 * BLE log via NUS
 *===================================================================*/

static void ble_log(const char * fmt, ...);

/* Live tuning over the Nordic UART Service (send from nRF Connect's UART tab).
 *   p1 / p0   P0.30 high / low
 *   e1 / e0   P1.12 high (receiver off) / low (receiver on)   E1/E0 = toggle it on open/close
 *   d         capture the next 96 raw serial bytes and dump them as hex
 *   h1 / h0   hardware flow control on/off (RTS P0.05, CTS P0.07)  — reopens UART
 *   t1 / t0   TXD P0.27 connected / floating                       — reopens UART
 *   o / x     open / close the UART
 *   r         reset counters
 *   ?         print the current state (+ the settings line)
 *   K<n> O<n> W<n> C<n> S Z   settings: power scale x1000, power offset dW,
 *             wheel mm, CSC mode, show, factory reset (see docs)
 *   B         clean reboot (applies a changed CSC mode)
 * Nothing here can transmit on the bus: TX is never used. */
/* Echo the live settings (every settings command answers with this line). */
static void settings_reply(char cmd, bool ok)
{
    if (!ok)
    {
        ble_log("ERR %c range", cmd);
        return;
    }
    const dfc_settings_t * st = settings_get();
    ble_log("SET scale=%u offset=%d wheel=%u csc=%u src=%s",
            (unsigned)st->power_scale_x1000, (int)st->power_offset_dw,
            (unsigned)st->wheel_mm, (unsigned)st->csc_mode, settings_source());
}

static void nus_data_handler(ble_nus_evt_t * p_evt)
{
    if (p_evt->type != BLE_NUS_EVT_RX_DATA || p_evt->params.rx_data.length < 1) return;

    char c = (char)p_evt->params.rx_data.p_data[0];
    char v = (p_evt->params.rx_data.length > 1) ? (char)p_evt->params.rx_data.p_data[1] : '0';
    bool on = (v == '1');

    /* Settings commands carry a signed decimal after the letter (K1100, O-15,
     * W2105, C0). Parsed on a NUL-terminated copy; no number = 0. */
    char    numbuf[24];
    uint16_t nlen = p_evt->params.rx_data.length - 1;
    if (nlen > sizeof(numbuf) - 1) nlen = sizeof(numbuf) - 1;
    memcpy(numbuf, p_evt->params.rx_data.p_data + 1, nlen);
    numbuf[nlen] = 0;
    long num = strtol(numbuf, NULL, 10);

    switch (c)
    {
        case 'K':
            settings_reply(c, settings_set_power(num, settings_get()->power_offset_dw));
            return;
        case 'O':
            settings_reply(c, settings_set_power(settings_get()->power_scale_x1000, num));
            return;
        case 'W':
            settings_reply(c, settings_set_wheel(num));
            return;
        case 'C':
            if (settings_set_csc(num))
            {
                settings_reply(c, true);
                ble_log("(csc mode %u saved; mode %u runs until reboot with B)",
                        (unsigned)settings_get()->csc_mode, (unsigned)m_csc_mode);
            }
            else settings_reply(c, false);
            return;
        case 'B':
            /* clean reboot (no fault record): applies a new csc mode */
            ble_log("REBOOT");
            nrf_delay_ms(50);   /* let the notification out */
            (void)sd_nvic_SystemReset();
            return;
        case 'Z':
            settings_reset();
            settings_reply(c, true);
            return;
        case 'S':
            settings_reply(c, true);
            return;
        case 'T':
        {
            /* resistance table in use: captured from this bike or the default */
            const uint16_t * t = peloton_rlut();
            ble_log("RLUT captured=%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
                    peloton_rlut_captured() ? 1 : 0,
                    t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
                    t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15]);
            nrf_delay_ms(20);
            ble_log("RLUT %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
                    t[16], t[17], t[18], t[19], t[20], t[21], t[22], t[23],
                    t[24], t[25], t[26], t[27], t[28], t[29], t[30]);
            return;
        }
        case '?':
            settings_reply(c, true);
            break;
        case 'p':
            if (on) NRF_P0->OUTSET = (1UL << SP3222_SHDN_PIN);
            else    NRF_P0->OUTCLR = (1UL << SP3222_SHDN_PIN);
            break;
        case 'e':
            /* e1 = P1.12 HIGH (receiver off), e0 = LOW (receiver on) */
            if (on) NRF_P1->OUTSET = (1UL << SP3222_EN_PIN);
            else    NRF_P1->OUTCLR = (1UL << SP3222_EN_PIN);
            break;
        case 'd':
            /* capture the next 96 raw bytes; dumped by the timer handler */
            m_raw_ready = false;
            m_raw_n = 0;
            break;
        case 'E':
            m_cfg_en_on_open = on;
            break;
        case 'h':
            m_cfg_hwfc = on;
            uart_close(); uart_open();
            break;
        case 't':
            m_cfg_txd = on;
            uart_close(); uart_open();
            break;
        case 'o':
            uart_open();
            break;
        case 'x':
            uart_close();
            break;
        case 'r':
            m_uart_rx_count = 0; m_uart_err_count = 0;
            peloton_init(TICKS_PER_SEC);
            break;
        case 'F':
            /* bench test of the fault path: expect a reset, then
             * rst=1 last=1:3@<line> (SDK error, NRF_ERROR_INTERNAL) */
            APP_ERROR_CHECK(NRF_ERROR_INTERNAL);
            break;
        case 'R':
            /* clear the fault record and the ignored-error counter */
            m_fault.count = 0; m_fault.id = 0; m_fault.pc = 0; m_fault.info = 0;
            m_sd_err = 0;
            break;
        default:
            break;
    }

    ble_log("P30=%d P112=%d hwfc=%d txd=%d uart=%d",
            (int)((NRF_P0->OUT >> SP3222_SHDN_PIN) & 1),
            (int)((NRF_P1->OUT >> SP3222_EN_PIN) & 1),
            (int)m_cfg_hwfc, (int)m_cfg_txd, (int)m_uart_enabled);
}

static void ble_log(const char * fmt, ...)
{
    if (conn_count() == 0) return;

    char buf[160];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0)
    {
        for (uint8_t i = 0; i < NRF_SDH_BLE_TOTAL_LINK_COUNT; i++)
        {
            if (m_conn[i] == BLE_CONN_HANDLE_INVALID) continue;
            uint16_t length = (uint16_t)len;
            (void)ble_nus_data_send(&m_nus, (uint8_t *)buf, &length, m_conn[i]);
        }
    }
}

/*===================================================================
 * Advertising
 *===================================================================*/

static void on_adv_evt(ble_adv_evt_t ble_adv_evt)
{
    switch (ble_adv_evt)
    {
        case BLE_ADV_EVT_FAST:
            NRF_LOG_INFO("Fast advertising.");
            break;
        case BLE_ADV_EVT_IDLE:
            /* Should not happen with timeout=0, but restart just in case */
            break;
        default:
            break;
    }
}

static void advertising_init(void)
{
    ret_code_t             err_code;
    ble_advertising_init_t init;

    memset(&init, 0, sizeof(init));

    /* --- Advertising data ---
     * Service UUIDs go in the primary packet so scanners filtering on the
     * Cycling Power UUID (Apple Watch, Garmin, Zwift) see us without needing
     * the scan response. The name moves to the scan response. */
    init.advdata.name_type               = BLE_ADVDATA_NO_NAME;
    init.advdata.include_appearance      = true;
    init.advdata.flags                   = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;

    /* FTMS service data: UUID(2) + flags(1) + type(2) = 5 bytes
     * Flags: 0x01 = Fitness Machine Available
     * Type: 0x0020 = Indoor Bike Supported (bit 5)
     *
     * Note: The service data AD type includes the UUID as part of the data,
     * but ble_advdata encodes UUID separately. We use raw service data matching
     * what the Arduino firmware sends: {UUID_HI, UUID_LO, 0x01, 0x00, 0x20} */
#if FTMS_ENABLED
    static uint8_t ftms_service_data[] = {0x01, 0x20, 0x00};
    ble_advdata_service_data_t service_data;
    service_data.service_uuid = BLE_UUID_FTMS;
    service_data.data.size    = sizeof(ftms_service_data);
    service_data.data.p_data  = ftms_service_data;

    init.advdata.p_service_data_array = &service_data;
    init.advdata.service_data_count   = 1;
#endif

    /* Service list by csc mode: 0 CPS, 1 CSC, 2 CPS+CSC; FTMS always. */
    static ble_uuid_t adv_uuids[3];
    uint8_t n = 0;
    if (m_csc_mode != 1) { adv_uuids[n].uuid = BLE_UUID_CPS; adv_uuids[n++].type = BLE_UUID_TYPE_BLE; }
    if (m_csc_mode != 0) { adv_uuids[n].uuid = BLE_UUID_CSC; adv_uuids[n++].type = BLE_UUID_TYPE_BLE; }
#if FTMS_ENABLED
    adv_uuids[n].uuid = BLE_UUID_FTMS; adv_uuids[n++].type = BLE_UUID_TYPE_BLE;
#endif
    init.advdata.uuids_complete.uuid_cnt = n;
    init.advdata.uuids_complete.p_uuids  = adv_uuids;

    init.srdata.name_type = BLE_ADVDATA_FULL_NAME;

    init.config.ble_adv_fast_enabled  = true;
    init.config.ble_adv_fast_interval = APP_ADV_FAST_INTERVAL;
    init.config.ble_adv_fast_timeout  = APP_ADV_FAST_DURATION;
    init.config.ble_adv_slow_enabled  = true;
    init.config.ble_adv_slow_interval = APP_ADV_SLOW_INTERVAL;
    init.config.ble_adv_slow_timeout  = 0;  /* Advertise forever */

    init.evt_handler = on_adv_evt;

    err_code = ble_advertising_init(&m_advertising, &init);
    APP_ERROR_CHECK(err_code);

    ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);
}

/*===================================================================
 * Connection parameters
 *===================================================================*/

static void on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
    if (p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED)
    {
        NRF_LOG_WARNING("Conn params failed — ignoring.");
    }
}

static void conn_params_error_handler(uint32_t nrf_error)
{
    NRF_LOG_WARNING("Conn params error: 0x%x — ignoring.", nrf_error);
}

static void conn_params_init(void)
{
    ret_code_t             err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));
    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = m_cps_power_meas_handles.cccd_handle;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}


/*===================================================================
 * Advertising start
 *===================================================================*/

static void advertising_start(void)
{
    ret_code_t err_code = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
    if (err_code != NRF_ERROR_INVALID_STATE) sd_check("adv_start", err_code);
}

/*===================================================================
 * Notification timer handler (fires every 500ms)
 *===================================================================*/

static void notify_timer_handler(void * p_context)
{
    (void)p_context;

    /* Blink LED to show firmware is alive.
     * Always toggle (500ms timer = 1Hz blink). */
    NRF_P0->OUT ^= (1UL << LED_PIN);

    if (conn_count() == 0)
    {
        return;
    }

    /* Log UART status periodically — both RTT and BLE NUS */
    static uint16_t diag_count = 0;
    diag_count++;

    /* Raw capture dump (one 16-byte line per tick until drained) */
    if (m_raw_ready)
    {
        static uint16_t dump_pos = 0;
        char line[40];
        int  n = 0;
        for (uint8_t i = 0; i < 16 && dump_pos + i < RAW_DUMP_LEN; i++)
            n += snprintf(line + n, sizeof(line) - n, "%02X", m_raw[dump_pos + i]);
        ble_log("D%02u %s", dump_pos / 16, line);
        dump_pos += 16;
        if (dump_pos >= RAW_DUMP_LEN) { dump_pos = 0; m_raw_ready = false; m_raw_n = RAW_DUMP_LEN; }
        return;
    }

    /* Send BLE log every 2 seconds (4 × 500ms) */
    if ((diag_count % 4) == 0)
    {
        const peloton_ride_t * r = peloton_ride();
        ble_log("rx=%lu ok=%lu bad=%lu(cs%lu f%lu l%lu i%lu d%lu) c=%u p=%u r=%u%% P30=%d P112=%d h%d t%d sd=%lu rst=%lu last=%lX:%lX@%lu dfu=%u ram=%lX fw=" FW_VERSION,
                (unsigned long)m_uart_rx_count,
                (unsigned long)r->frames_ok,
                (unsigned long)r->frames_bad,
                (unsigned long)r->bad_cs, (unsigned long)r->bad_footer,
                (unsigned long)r->bad_len, (unsigned long)r->bad_id, (unsigned long)r->bad_digit,
                peloton_cadence(),
                peloton_watts(),
                peloton_resistance_pct(),
                (int)((NRF_P0->OUT >> SP3222_SHDN_PIN) & 1),
                (int)((NRF_P1->OUT >> SP3222_EN_PIN) & 1),
                (int)m_cfg_hwfc, (int)m_cfg_txd,
                (unsigned long)m_sd_err, (unsigned long)m_fault.count,
                (unsigned long)m_fault.id, (unsigned long)m_fault.pc, (unsigned long)m_fault.info,
                (unsigned)m_dfu_ready, (unsigned long)m_ram_min);
    }

    if ((diag_count % 10) == 0)
    {
        NRF_LOG_INFO("rx=%d ok=%d cad=%d pow=%dW res=%d%%",
                     m_uart_rx_count, peloton_ride()->frames_ok,
                     peloton_cadence(), peloton_watts(), peloton_resistance_pct());
    }

    peloton_expire_stale(app_timer_cnt_get());

    uint16_t power_w = peloton_watts();
    uint16_t cadence = peloton_cadence();
    uint16_t resist  = peloton_resistance_pct();
    uint16_t speed   = peloton_speed_kmh_x100(SPEED_FROM_POWER, WHEEL_CIRCUMFERENCE, GEAR_RATIO);

    /* --- CPS Power Measurement: flags + power + wheel data + crank data --- */
    if (m_csc_mode != 1)
    {
        uint8_t  buf[14];
        uint16_t len = sizeof(buf);

        /* Flags: bit 4 = Wheel Revolution Data Present,
         *        bit 5 = Crank Revolution Data Present */
        buf[0] = 0x30;
        buf[1] = 0x00;
        buf[2] = (uint8_t)(power_w & 0xFF);
        buf[3] = (uint8_t)((power_w >> 8) & 0xFF);

        /* Cumulative wheel revolutions (uint32) + last wheel event time
         * (uint16, 1/2048 s) — speed and distance for the client. */
        uint32_t wheel_revs = peloton_wheel_revs();
        uint16_t wheel_time = peloton_wheel_event_time();
        buf[4] = (uint8_t)(wheel_revs & 0xFF);
        buf[5] = (uint8_t)((wheel_revs >> 8) & 0xFF);
        buf[6] = (uint8_t)((wheel_revs >> 16) & 0xFF);
        buf[7] = (uint8_t)((wheel_revs >> 24) & 0xFF);
        buf[8] = (uint8_t)(wheel_time & 0xFF);
        buf[9] = (uint8_t)((wheel_time >> 8) & 0xFF);

        /* Cumulative crank revolutions (uint16) + last crank event time
         * (uint16, 1/1024 s) — cadence for the client. */
        uint16_t crank_revs = peloton_crank_revs();
        uint16_t crank_time = peloton_crank_event_time();
        buf[10] = (uint8_t)(crank_revs & 0xFF);
        buf[11] = (uint8_t)((crank_revs >> 8) & 0xFF);
        buf[12] = (uint8_t)(crank_time & 0xFF);
        buf[13] = (uint8_t)((crank_time >> 8) & 0xFF);

        ble_gatts_hvx_params_t hvx_params;
        memset(&hvx_params, 0, sizeof(hvx_params));
        hvx_params.handle = m_cps_power_meas_handles.value_handle;
        hvx_params.type   = BLE_GATT_HVX_NOTIFICATION;
        hvx_params.offset = 0;
        hvx_params.p_data = buf;

        for (uint8_t i = 0; i < NRF_SDH_BLE_TOTAL_LINK_COUNT; i++)
        {
            if (m_conn[i] == BLE_CONN_HANDLE_INVALID) continue;
            len = sizeof(buf);
            hvx_params.p_len = &len;
            sd_ble_gatts_hvx(m_conn[i], &hvx_params);
        }
    }

    /* --- CSC Measurement: flags + wheel data (1/1024 s) + crank data --- */
    if (m_csc_mode != 0)
    {
        uint8_t  buf[11];
        uint16_t len;
        uint32_t wheel_revs = peloton_wheel_revs();
        uint16_t wheel_time = peloton_wheel_event_time_1024();
        uint16_t crank_revs = peloton_crank_revs();
        uint16_t crank_time = peloton_crank_event_time();
        buf[0]  = 0x03;   /* wheel + crank revolution data present */
        buf[1]  = (uint8_t)(wheel_revs & 0xFF);
        buf[2]  = (uint8_t)((wheel_revs >> 8) & 0xFF);
        buf[3]  = (uint8_t)((wheel_revs >> 16) & 0xFF);
        buf[4]  = (uint8_t)((wheel_revs >> 24) & 0xFF);
        buf[5]  = (uint8_t)(wheel_time & 0xFF);
        buf[6]  = (uint8_t)((wheel_time >> 8) & 0xFF);
        buf[7]  = (uint8_t)(crank_revs & 0xFF);
        buf[8]  = (uint8_t)((crank_revs >> 8) & 0xFF);
        buf[9]  = (uint8_t)(crank_time & 0xFF);
        buf[10] = (uint8_t)((crank_time >> 8) & 0xFF);

        ble_gatts_hvx_params_t hvx_params;
        memset(&hvx_params, 0, sizeof(hvx_params));
        hvx_params.handle = m_csc_meas_handles.value_handle;
        hvx_params.type   = BLE_GATT_HVX_NOTIFICATION;
        hvx_params.p_data = buf;
        for (uint8_t i = 0; i < NRF_SDH_BLE_TOTAL_LINK_COUNT; i++)
        {
            if (m_conn[i] == BLE_CONN_HANDLE_INVALID) continue;
            len = sizeof(buf);
            hvx_params.p_len = &len;
            sd_ble_gatts_hvx(m_conn[i], &hvx_params);
        }
    }

#if FTMS_ENABLED
    /* --- FTMS Indoor Bike Data notification (speed + cadence + resistance + power) --- */
    {
        uint8_t  buf[10];
        uint16_t len = sizeof(buf);

        /* Flags: 0x0064
         * bit 0 = 0: speed present
         * bit 2 = 1: cadence present
         * bit 5 = 1: resistance present
         * bit 6 = 1: power present */
        buf[0] = 0x64;
        buf[1] = 0x00;

        /* Instantaneous speed, km/h * 100 */
        buf[2] = (uint8_t)(speed & 0xFF);
        buf[3] = (uint8_t)((speed >> 8) & 0xFF);

        /* Instantaneous cadence, 0.5 rpm units */
        uint16_t cadence_ftms = (uint16_t)(cadence * 2);
        buf[4] = (uint8_t)(cadence_ftms & 0xFF);
        buf[5] = (uint8_t)((cadence_ftms >> 8) & 0xFF);

        /* Resistance level, unitless: Peloton 0..100 % */
        buf[6] = (uint8_t)(resist & 0xFF);
        buf[7] = (uint8_t)((resist >> 8) & 0xFF);

        /* Instantaneous power, W */
        buf[8] = (uint8_t)(power_w & 0xFF);
        buf[9] = (uint8_t)((power_w >> 8) & 0xFF);

        ble_gatts_hvx_params_t hvx_params;
        memset(&hvx_params, 0, sizeof(hvx_params));
        hvx_params.handle = m_ftms_indoor_bike_handles.value_handle;
        hvx_params.type   = BLE_GATT_HVX_NOTIFICATION;
        hvx_params.offset = 0;
        hvx_params.p_data = buf;

        for (uint8_t i = 0; i < NRF_SDH_BLE_TOTAL_LINK_COUNT; i++)
        {
            if (m_conn[i] == BLE_CONN_HANDLE_INVALID) continue;
            len = sizeof(buf);
            hvx_params.p_len = &len;
            sd_ble_gatts_hvx(m_conn[i], &hvx_params);
        }
    }
#else
    (void)cadence; (void)resist; (void)speed;
#endif

}

/*===================================================================
 * Main
 *===================================================================*/

/*===================================================================
 * Peer Manager (bonding)
 *===================================================================*/

static void pm_evt_handler(pm_evt_t const * p_evt)
{
    /* The SDK helpers pm_handler_on_pm_evt / pm_handler_flash_clean /
     * pm_handler_disconnect_on_sec_failure all contain APP_ERROR_CHECK,
     * so they are replaced by this handler: log everything, reply where
     * a reply is required, recover storage, never fault, never drop a
     * link because pairing failed (the CPS/FTMS characteristics are open,
     * data still flows unencrypted). */
    pm_handler_pm_evt_log(p_evt);

    switch (p_evt->evt_id)
    {
        case PM_EVT_CONN_SEC_SUCCEEDED:
            ble_log("EVT sec ok h=%u proc=%u peer=%u", p_evt->conn_handle,
                    p_evt->params.conn_sec_succeeded.procedure, p_evt->peer_id);
            break;
        case PM_EVT_CONN_SEC_FAILED:
            ble_log("EVT sec FAIL h=%u err=0x%04X src=%u", p_evt->conn_handle,
                    p_evt->params.conn_sec_failed.error, p_evt->params.conn_sec_failed.error_src);
            break;
        case PM_EVT_CONN_SEC_CONFIG_REQ:
        {
            /* A bonded peer wants to pair again (Garmin after a firmware
             * update, #33): allow it. */
            pm_conn_sec_config_t cfg = { .allow_repairing = true };
            ble_log("EVT sec re-pair req h=%u (allowed)", p_evt->conn_handle);
            pm_conn_sec_config_reply(p_evt->conn_handle, &cfg);
            break;
        }
        case PM_EVT_CONN_SEC_PARAMS_REQ:
            /* NULL = use the parameters given to pm_sec_params_set(). */
            sd_check("pm_sec_params_reply",
                     pm_conn_sec_params_reply(p_evt->conn_handle, NULL,
                                              p_evt->params.conn_sec_params_req.p_context));
            break;
        case PM_EVT_STORAGE_FULL:
        case PM_EVT_FLASH_GARBAGE_COLLECTION_FAILED:
            ble_log("EVT pm storage gc");
            sd_check("fds_gc", fds_gc());
            break;
        case PM_EVT_PEER_DATA_UPDATE_FAILED:
            ble_log("EVT pm update FAIL err=0x%lX", (unsigned long)p_evt->params.peer_data_update_failed.error);
            if (p_evt->params.peer_data_update_failed.error == NRF_ERROR_STORAGE_FULL)
                sd_check("fds_gc", fds_gc());
            break;
        case PM_EVT_ERROR_UNEXPECTED:
            ble_log("EVT pm ERR 0x%lX", (unsigned long)p_evt->params.error_unexpected.error);
            break;
        case PM_EVT_PEERS_DELETE_SUCCEEDED:
            NRF_LOG_INFO("Bonds deleted.");
            break;
        default:
            break;
    }
}

static void peer_manager_init(void)
{
    ble_gap_sec_params_t sec_param;
    ret_code_t           err_code;

    err_code = pm_init();
    APP_ERROR_CHECK(err_code);

    memset(&sec_param, 0, sizeof(ble_gap_sec_params_t));
    sec_param.bond           = SEC_PARAM_BOND;
    sec_param.mitm           = SEC_PARAM_MITM;
    sec_param.lesc           = SEC_PARAM_LESC;
    sec_param.keypress       = SEC_PARAM_KEYPRESS;
    sec_param.io_caps        = SEC_PARAM_IO_CAPABILITIES;
    sec_param.oob            = SEC_PARAM_OOB;
    sec_param.min_key_size   = SEC_PARAM_MIN_KEY_SIZE;
    sec_param.max_key_size   = SEC_PARAM_MAX_KEY_SIZE;
    sec_param.kdist_own.enc  = 1;
    sec_param.kdist_own.id   = 1;
    sec_param.kdist_peer.enc = 1;
    sec_param.kdist_peer.id  = 1;

    err_code = pm_sec_params_set(&sec_param);
    APP_ERROR_CHECK(err_code);

    err_code = pm_register(pm_evt_handler);
    APP_ERROR_CHECK(err_code);
}

/*===================================================================
 * Fault handler: fast LED blink forever so a crash is visible without a
 * debugger (the SDK default silently resets, which looks like a dead board).
 *===================================================================*/

void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info)
{
    __disable_irq();
    if (m_fault.magic != FAULT_MAGIC)
    {
        m_fault.magic = FAULT_MAGIC;
        m_fault.count = 0;
    }
    m_fault.count++;
    m_fault.id = id;
    if (id == NRF_FAULT_ID_SDK_ERROR && info != 0)
    {
        error_info_t const * e = (error_info_t const *)info;
        m_fault.pc   = e->err_code;
        m_fault.info = e->line_num;
    }
    else if (id == NRF_FAULT_ID_SDK_ASSERT && info != 0)
    {
        assert_info_t const * a = (assert_info_t const *)info;
        m_fault.pc   = pc;
        m_fault.info = a->line_num;
    }
    else
    {
        m_fault.pc   = pc;
        m_fault.info = info;
    }
    /* Reset instead of halting: a halted DFC is dead until power-cycle,
     * a reset is advertising again within a second. */
    NVIC_SystemReset();
}

/*===================================================================
 * GPIO setup (bench-verified 2026-10-12):
 *   P0.28 LED on
 *   P0.30 output LOW   — never HIGH: that freezes the bike
 *   P1.12 output HIGH  — receiver off while idle; uart_open() drives it LOW
 *   P0.25 input with pull-up (SP3222 D2IN, keeps the unused driver quiet)
 *===================================================================*/

static void gpio_init(void)
{
    NRF_P0->PIN_CNF[LED_PIN] = 3;
    NRF_P0->OUTSET = (1UL << LED_PIN);

    NRF_P0->OUTCLR = (1UL << SP3222_SHDN_PIN);
    NRF_P0->PIN_CNF[SP3222_SHDN_PIN] = 3;

    NRF_P1->OUTSET = (1UL << SP3222_EN_PIN);
    NRF_P1->PIN_CNF[SP3222_EN_PIN] = 3;

    NRF_P0->PIN_CNF[P0_25_PIN] = 0x0003000C;
}

int main(void)
{
    gpio_init();
    fault_record_init();
    peloton_init(TICKS_PER_SEC);

    /* Initialize modules */
    log_init();
    timers_init();
    power_management_init();
    /* must run before the SoftDevice is enabled; fails without a bootloader */
    m_dfu_ready = (ble_dfu_buttonless_async_svci_init() == NRF_SUCCESS);
    ble_stack_init();
    peer_manager_init();            /* pm_init initialises FDS */
    settings_init();                /* reads the record (waits for FDS); applies calibration */
    m_csc_mode = settings_get()->csc_mode;   /* decides appearance, services, advertising */
    gap_params_init();
    gatt_init();
    services_init();
    advertising_init();
    conn_params_init();

    /* UART opens on BLE connect, closes on disconnect — matches the
     * original firmware. The SP3222 driver is already shut down by
     * gpio_init(), so nothing we do can reach the Peloton bus. */

    if (LISTEN_ALWAYS) uart_open();

    /* Start advertising and notification timer */
    advertising_start();
    timers_start();

    NRF_LOG_INFO("DFC BLE firmware started. Advertising as \"%s\".", DEVICE_NAME);

    /* Main loop: everything happens in the UART ISR (parser) and the
     * app_timer handler (BLE notifications). Sleep in between. */
    for (;;)
    {
        sd_app_evt_wait();
    }
}
