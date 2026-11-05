# Peloton Serial Protocol & DFC Signal Path

Reference for the firmware in `src/nordic/`. Protocol decoded by Imran Haque
([PeloMon, Part I](https://ihaque.org/posts/2020/10/15/pelomon-part-i-decoding-peloton/),
[code](https://github.com/ihaque/pelomon)); hardware verified against the Rev D
schematic and the SP3222E datasheet on 2026-10-05.

## Physical layer

| Item | Value |
|---|---|
| Levels | RS-232, about ±5.5 V (NOT TTL — never connect a GPIO directly) |
| Rate | 19200 baud, 8N1 |
| Jack TIP | head unit → bike (requests) |
| Jack RING | bike → head unit (responses) — **this is what DFC listens to** |
| Jack SLEEVE | ground |

On the DFC both 3.5 mm jacks are wired in parallel (TIP↔TIP, RING↔RING), so the
board is a pass-through tap. RING feeds SP3222 receiver 1 (`RIN1`), whose output
`R1OUT` is P0.26. The SP3222 driver output `DOUT1` only reaches TIP through
jumper JP7 / solder jumper JP8, both open by default.

## Transceiver control pins (bench-verified on a live bike, 2026-10-12)

Each signal was toggled live over the BLE UART while pedaling, watching the
firmware's byte counter and the tablet. Result:

| nRF pin | Net | Required state | Effect of the other state |
|---|---|---|---|
| P0.30 | 3222_PWRDN | **LOW**, always | reception stops at once and the tablet freezes (RS-232 driver active on the bus) |
| P1.12 | 3222_EN | **LOW** while receiving (HIGH when idle) | reception stops at once |
| P0.26 | R1OUT | UART RXD | |
| P0.27 / P0.05 / P0.07 | TXD / RTS / CTS | not connected to the UART | not needed; the board only listens |
| P0.25 | D2IN | input with pull-up | keeps the unused second driver quiet |

Two earlier readings pointed the other way and were wrong: a bench test with
the "passive" polarity that received nothing (invalid: the J-Link was attached
alongside the bike cable, which halts the board), and a reading of the original
firmware's disassembly. Trust the live toggle test.

Practical rules: never debug over SWD with the bike cable plugged in, and
power the board from a USB-A source (a USB-C to USB-C charger leaves it dead).

This bike's head unit polls roughly 30 times per second (about 265 bytes/s on
the bike-to-head-unit line), faster than the 10/s PeloMon observed.

## Frames

Head unit → bike, 4 bytes:

```
F5 <id> <cs> F6
```

Bike → head unit:

```
F1 <id> <len> <len ASCII digits, LEAST significant first> <cs> F6
cs = (F1 + id + len + digits...) mod 256
```

| id | len | Meaning | Example payload | Value |
|---|---|---|---|---|
| `41` | 3 | cadence, rpm | `34 38 30` ("480") | 84 rpm |
| `44` | 5 | power, deciwatts | `30 35 35 31 30` ("05510") | 155.0 W |
| `4A` | 4 | resistance, raw sensor | `38 36 36 30` ("8660") | 668 raw |
| `49` | 3 | resistance level, 0–100 % as computed by the bike (sent by the current Peloton software, polled every cycle; not in PeloMon's 2020 captures) | `32 32 30` ("220") | 22 % |
| `F7` | 4 | resistance-table entry | | one of 31 |
| `FB` | 7 | bike ID | | boot only |
| `FE` | 3 | init reply | | boot only |

PeloMon's 2020 bike polled `41`, `44`, `4A` round-robin every ~100 ms. The bike
captured on 2026-10-12 polls `44`, `4A`, `49`, `41` in a loop about 30 times per
second (each value refreshes ~7 times a second). Example captured cycle:

```
F1 44 05 39 31 31 30 30 35 F6   power  "00119" -> 11.9 W
F1 4A 04 36 35 34 30 0E F6      raw resistance 0456
F1 49 03 32 32 30 D1 F6         resistance level 22 %
F1 41 03 39 34 30 D2 F6         cadence 49 rpm
``` At head-unit boot it sends `FE 00`,
`F5 FB`, then `F7 00` … `F7 1E` (31 resistance calibration points).

## Resistance 0–100 %

The head unit maps raw resistance to the displayed 0–100 % by linear
interpolation across the 31 calibration points (point *i* = *i* × 100/30 %).
When the bike sends `49`, the firmware reports that value directly and ignores
the table. Otherwise: the `F7` reply does not carry the index, so the firmware captures the table
only when it sees 31 ascending entries in one burst (≤ 1 s apart, no other
frame in between). Until then it uses the built-in default, which is the table
captured from the author's original Bike (`163 169 186 226 289 350 412 478 534 580
622 663 698 728 756 780 803 822 839 855 870 881 892 904 913 921 930 936 943 948
953`; PeloMon's 2020 bike had `164 169 186 222 297 … 967`, a few percent apart
in the middle). Because the firmware only listens while a BLE client is
connected, a bike's own table is captured only if a client is connected while
the tablet boots; `T` prints the table in use and whether it was captured
(verified 2026-10-15: `captured=0` with the default, then `captured=1` with
the bike's values after a tablet restart with the bench client connected).

## Speed

The Peloton head unit derives speed from power, not from cadence (PeloMon's
observation). The firmware reproduces that curve for FTMS speed and for the
CPS wheel data: two cubics in √W with a break at 27 W, fitted from the owner's
own rides. Set `SPEED_FROM_POWER 0` in `main.c` to fall back to cadence × fixed
gear.

The fit (2026-10-14): 87 of the owner's rides from 2016 to 2026, exported per
second from their Peloton workout history, 45 685 (W, mph) samples from 1 to
499 W. `scripts/fit_speed.py` takes the median displayed speed per watt (equal
weight per watt, so the sparse high-power end counts) and solves the least
squares; the two-cubic form beats a single cubic (0.034 vs 0.067 mph RMS on the
per-watt table) and sits at the display's 0.1 mph rounding floor (0.046 mph RMS
on the raw samples). Residuals show no drift across the ten years of rides.
Integrating the 2026-10-14 ride capture with the curve gives 1.374 mi against
the Peloton summary's 1.36 mi.

Verified 2026-10-15 on 0.2.0-dev6 (8-minute ride, 26–292 W): the curve
reproduces the per-second speed of that ride's Peloton export with 0.034 mph
RMS and +0.008 mph mean; integrating the export's power through the curve
gives 2.174 mi, the export's own speed column 2.175 mi, the Apple Watch
2.18 mi (110 W / 75 rpm / 16.0 mph average over 8:10 vs the Peloton summary's
111 W / 76 rpm / 2.17 mi over 8:02).
To re-fit from your own rides, export each ride as a CSV with one row per
second and the columns `t_s,output_w,cadence_rpm,resistance_pct,speed,speed_unit`
(speed as the bike displays it, `mph` or `km/h`), then:

```
python3 scripts/fit_speed.py fit 'captures/*.csv' --check <bench log> <peloton miles>
```

## What the firmware broadcasts

| Service | Characteristic | Contents |
|---|---|---|
| Cycling Power 0x1818 | Power Measurement 0x2A63 | flags 0x0030: power (W), cumulative wheel revs + last wheel event (1/2048 s), cumulative crank revs + last crank event (1/1024 s) |
| | Feature 0x2A65 | 0x0000000C: wheel + crank revolution data supported |
| | Sensor Location 0x2A5D | 5 (left crank) |
| | Control Point 0x2A66 | write + indicate; every op code answered "not supported" |
| Fitness Machine 0x1826 | Indoor Bike Data 0x2AD2 | flags 0x0064: speed (0.01 km/h), cadence (0.5 rpm), resistance (0–100), power (W) |
| | Feature 0x2ACC | cadence, resistance level, power |
| Device Information | manufacturer, model, firmware, hardware revision | |
| Battery | level (fixed 100) | |
| Nordic UART | TX: diagnostic line every 2 s + BLE event log; RX: tuning commands | see below |

GAP appearance 0x0484 (Cycling Power Sensor). Up to two clients at once
(e.g. Apple Watch + a phone app). Just-Works bonding via Peer Manager.

Crank revolution data gives clients cadence; wheel revolution data gives
them speed and distance (Apple Watch uses it like a PowerTap hub — set the
wheel size to 700x25c / 2105 mm on the client). Both are synthesized from
the bike's cadence and from the power-derived speed. Values are zeroed if
the bus is silent for 5 s.

Verified 2026-10-12 on a 5-minute ride: Apple Watch 109 W / 78 rpm /
16.4 mph / 1.37 mi vs Peloton 110 W / 79 rpm / 1.40 mi.
Verified again 2026-10-14 (0.2.0-dev2, installed over the air, Watch
re-paired after the bond page moved): Apple Watch 102 W / 75 rpm / 15.9 mph /
1.34 mi vs Peloton 104 W / 76 rpm / 1.36 mi over 5:03; 13 161 frames, none
bad, no SoftDevice errors, no resets.
Verified 2026-10-15 (0.2.0-dev6, own speed curve and resistance table):
Apple Watch 110 W / 75 rpm / 16.0 mph / 2.18 mi over 8:10 vs Peloton 111 W /
76 rpm / 40 % / 2.17 mi over 8:02; the Wahoo app connected as the second client
after the bench client left and recorded 1:39 of live data (143 W / 82 rpm /
18.2 mph / 0.50 mi, its .fit matches); 11 135 frames, none bad, no SoftDevice
errors, no resets.

### Diagnostic channel (Nordic UART Service)

Every 2 s:
`rx=<bytes> ok=<frames> bad=<n>(cs f l i d) c=<rpm> p=<W> r=<%> P30=<0|1> P112=<0|1> h<hwfc> t<txd> sd=<n> rst=<n> last=<id>:<err>@<line> dfu=<0|1> ram=<hex> fw=<version>`

Since 0.2.0 the board advertises as `DFC` (the stock firmware's name; development builds used `Coffee`). Verified 2026-10-15 after the 0.2.0 OTA: `sd=0 rst=0 dfu=1 ram=20003F10 fw=0.2.0`, settings at defaults, services 180a 180f 1818 1826 fe59.
plus `EVT ...` lines for connections (with the initial connection
parameters), disconnections (reason), connection parameter updates, PHY /
data-length / MTU requests, pairing steps, CCCD subscriptions and
`EVT ? id=0x..` for any event the firmware does not know, and `ERR <call>
0x<code>` whenever a SoftDevice call made in response to a peer request
fails.

`sd` counts SoftDevice errors ignored since boot. `rst` counts faults
(APP_ERROR_CHECK, assert, SoftDevice assert) since the last power-on: the
board resets instead of halting and keeps the record in no-init RAM;
`last` is the fault id, the error code (or PC) and the source line, e.g.
`last=4001:3@1206` for an SDK error (0x4001) with NRF_ERROR_INTERNAL (3) at
main.c line 1206; a line of 0 means the check sits inside SDK code. `dfu` is 1
when a bootloader is present and the Buttonless DFU service can hand over to
it; `ram` is the minimum application RAM start the SoftDevice reported at
boot (the linker origin must stay at or above it); `fw` is the build version
(confirms an OTA landed).

Bench-verified 2026-10-13/14: `F` resets the board within a second and the
next status line shows `rst=1 last=4001:3@1206`; `R` clears it. An OTA from
0.2.0-dev to 0.2.0-dev2 through nRF Connect on iOS took one pass with the
default settings.

Commands (send as text): `p0/p1` P0.30, `e0/e1` P1.12, `h0/h1` flow control,
`t0/t1` TXD, `o/x` UART open/close, `r` reset counters, `R` clear the fault
record, `F` force a fault (the board resets; tests the trace), `d` dump the
next 96 raw bytes as hex, `?` print state. Nothing can transmit on the bus.

### Settings

Owner settings live in one record in the FDS pages at 0xF5000 (file 0x0DFC,
record 0x0001), next to the bonds, so they survive resets and OTA updates.
Set them over the same NUS channel; every settings command answers with the
settings line, or `ERR <cmd> range` when a value is rejected and nothing is
stored:

`SET scale=<x1000> offset=<dW> wheel=<mm> csc=<mode> src=<default|fds|ram>`

| Command | Meaning | Range | Default |
|---|---|---|---|
| `K<n>` | power scale ×1000 (`K1100` = ×1.10) | 500–2000 | 1000 |
| `O<n>` | power offset in deciwatts, added after scaling (`O-15` = −1.5 W) | −500–500 | 0 |
| `W<n>` | wheel circumference in mm, for CPS wheel revolutions (speed/distance on the Watch) | 500–3000 | 2105 (700x25c) |
| `C<n>` | CSC mode: 0 CPS with wheel data, 1 CSC only, 2 both — stored now, acted on from the CSC build | 0–2 | 0 |
| `S` | show the settings line | | |
| `Z` | factory reset: defaults, record deleted | | |
| `T` | print the resistance table in use: `RLUT captured=<0|1>` (1 = read from this bike at tablet boot) and the 31 raw values | | |

`src` is `default` when no record exists, `fds` once the record has been read
or written, and `ram` while a change is still being written (or the write
failed; the value is in effect either way). `?` prints the line too.

Calibration: the tablet applies a per-bike factor that the bus does not
carry (upstream #11, #36). The DFC applies `scale` and `offset` to the bike's
deciwatts before anything else sees them, so power, the power-derived speed
and the wheel distance all follow the number on the Peloton screen. To
calibrate, ride at a steady output, compare the Watch with the screen and set
`K` = screen / DFC × 1000 (and `O` for a constant difference). Use the wheel
size that the Watch (or app) is configured with.

Verified 2026-10-14 on 0.2.0-dev3 (installed over the air with the bike
cable attached, bike idle): defaults reported, `K1100 O-15 W2000 C1` echoed
with `src=ram` then `src=fds`, `K9` rejected, values intact after an `F`
reset, `Z` back to defaults and still default after another reset. Live
calibration of bus power is covered by the host tests (the bike was not in
ride mode during the check).
Live calibration verified 2026-10-15 (0.2.0-dev6): with `K1100` sent at
minute 4 of a ride, the power the board reported over FTMS was 1.108 × the
bike's own per-second output for that minute (aligned Peloton export), 1.01 ×
before and 1.006 × after `Z`; `Z` answered `src=default` and the board stayed
at defaults.

### Sensor mode (CSC)

Some watches accept a speed/cadence sensor but not a power meter, or the
other way round (upstream #15 Venu 3 wants CSC, #35 Venu 4 works with CPS;
the developer shipped two firmware variants for this). The `csc` setting
chooses the GATT shape; it is read once at boot, so change it with `C<n>`
and reboot with `B`:

| Mode | Services | Advertised UUIDs | Appearance | For |
|---|---|---|---|---|
| 0 (default) | CPS (with wheel + crank data), FTMS | 1818 1826 | 0x0484 Cycling Power Sensor | Apple Watch, Wahoo, Zwift, Garmin Venu 4 |
| 1 | CSC, FTMS (no CPS) | 1816 1826 | 0x0485 Speed and Cadence Sensor | Garmin devices that refuse a power sensor (Venu 3) |
| 2 | CPS, CSC, FTMS | 1818 1816 1826 | 0x0484 | unknown devices; try this first |

CSC Measurement (0x2A5B, notify every 500 ms): flags 0x03, cumulative wheel
revolutions (uint32), last wheel event time (uint16, 1/1024 s), cumulative
crank revolutions (uint16), last crank event time (uint16, 1/1024 s); the
counts are the same ones the CPS packet carries (CPS wheel time is 1/2048 s).
CSC Feature reads 0x0003 (wheel + crank), Sensor Location 12 (rear wheel).
DIS, NUS and DFU are present in every mode.

Verified 2026-10-14 on 0.2.0-dev4 (bike idle): mode 0 lists 180a 180f 1818
1826 fe59; `C2` + `B` → 1816 added, CSC and CPS both notify, advertising
1818 1816 1826; `C1` + `B` → no 1818, CSC notifies, advertising 1816 1826;
`C0` + `B` → back to the default set. `B` reboots without a fault record
(`rst=0` throughout). Appearance was not read back (macOS hides it); counts
were zero because the bike was not in ride mode.

### Over-the-air update

The firmware exposes the Nordic Secure DFU service (FE59, buttonless). With
the open bootloader from `src/nordic/bootloader/` installed, nRF Connect
installs `dfc_update.zip` (`make dfu_pkg`); the board reappears as `DfuTarg`
during the update. Do it with the bike cable unplugged: the SP3222 control
pins float while the bootloader runs. Stock units need a package signed by
the upstream developer.

### Robustness (phase 4)

Every peer-initiated request is answered or ignored; none can reach an
error check. Upstream shipped exactly this change in `power-cadence-beta.4`
to stop the Garmin 16.13 / Edge 21.09 reset loop (intelligenate/dfc #19,
#24, #25, #26), together with the synthesised 100 ppm low-frequency clock
and 20–1000 ms / 10 s connection parameters that cured the Fenix 6 dropouts
(#2, #5). Both are reproduced here from the shipped binaries (connection
parameters, clock source and event handling read from the beta.4 image).

## Unit tests

`src/nordic/peloton.c` has no SDK dependencies. Test it on the host:

```
cd src/nordic/tests
cc -std=c11 -Wall -Wextra -I.. -o /tmp/test_peloton test_peloton.c ../peloton.c -lm
/tmp/test_peloton [path/to/pelomon/peloton_decoding/example-ride.txt]
```

## Sources and credits

This firmware stands on other people's work. In rough order of how much it mattered:

- **Intelligenate / DFC** — [github.com/intelligenate/dfc](https://github.com/intelligenate/dfc): the open hardware design (schematic, Gerbers), user guide, compatibility tables and the released firmware packages whose behaviour we reverse-engineered. The DFC is their product; this is an independent, unofficial firmware.
- **Imran Haque, PeloMon** — [Part I: decoding the Peloton](https://ihaque.org/posts/2020/10/15/pelomon-part-i-decoding-peloton/), [Part II](https://ihaque.org/posts/2020/12/26/pelomon-part-ii-emulating-peloton/), [Part III](https://ihaque.org/posts/2020/12/28/pelomon-part-iii-hardware/) and [github.com/ihaque/pelomon](https://github.com/ihaque/pelomon): the serial protocol (framing, LSB-first ASCII digits, checksum, message ids, resistance table), the physical-layer pinout, the resistance-table handshake at head-unit boot and the observation that the head unit's speed is a piecewise cubic in √W. The PeloMon firmware is CC-BY-NC 4.0; this firmware reproduces none of its code or data (the default table was captured from the author's bike, the speed coefficients are fitted from the author's rides).
- **MaxLinear SP3222E/SP3232E datasheet** — pin functions, shutdown/enable truth table, "driver inputs have no pull-ups".
- **Nordic Semiconductor** — nRF5 SDK 16.0.0 (ble_app_hrs, ble_app_uart, Peer Manager, nrfx_uart, secure bootloader examples), S140 SoftDevice 7.0.1, nrfjprog/nrfutil, and the DevZone Q&A on Cycling Power implementations.
- **Bluetooth SIG** — Cycling Power Service/Profile 1.1, Cycling Speed and Cadence Service, Fitness Machine Service 1.0, GATT assigned numbers (appearance 0x0484).
- **Apple** — watchOS 10 Workout sensor support (Apple Newsroom, Apple Support "What's new in Workout"); plus the write-ups by DC Rainmaker, the5krunner, 9to5Mac, BikeRadar and road.cc on how the Watch pairs power meters and derives cadence/speed from them.
- **kochcodes/ESP32_BLE_CyclingPowerMeter** and an Apple Developer Forums thread on Cycling Power on watchOS — consulted while chasing the Watch's behaviour.
- **SEGGER** (J-Link Commander, J-Link EDU Mini), **Adafruit** (#2094 adapter, EDU Mini), **Tag-Connect** (TC2030) — debug tooling and wiring references.
- **bleak** — the Python BLE library behind `scripts/dfc_ble.py`.
- Firmware, decoder, tests, tooling and documentation were written in this project with Claude (Anthropic) as pair programmer.

## Licensing note

Earlier builds reproduced PeloMon's default resistance table and speed
coefficients (CC-BY-NC 4.0). Since 0.2.0-dev6 the default table is the one
captured from the author's bike and the speed curve is fitted from the
author's own rides, so the firmware carries no CC-BY-NC material; PeloMon is
credited for the protocol work and the observations that made both possible.
