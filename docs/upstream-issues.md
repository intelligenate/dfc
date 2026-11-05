# Upstream issues and what firmware 0.2.0 does about them

The numbers below are issues of [intelligenate/dfc](https://github.com/intelligenate/dfc/issues), the DFC's own repository. This fork's firmware is independent and unofficial; this page says, for each issue, what 0.2.0 changes, how that was verified, or why it is not addressed. Verification state as of 2026-10-15: Apple Watch and the Wahoo app on a 2026 Peloton Bike, bench tests over Bluetooth, no Garmin hardware. Where an answer says *Garmin test pending*, the test is described in [developer-packaging.md](developer-packaging.md) and waits for the upstream developer or an owner with the device.

## The four clusters

**Garmin connection drops and "connected but no data"** (#2, #5, #19, #24, #25, #26, #27, #33). Upstream traced the 2024 cases to a Garmin software update that sends a request the firmware did not handle, and fixed it in `power-cadence-beta.4` by ignoring that request; the Fenix 6 dropouts were a clock and connection-interval problem. 0.2.0 reproduces beta.4's clock and connection parameters exactly (read from the released binary) and goes further: every request a peer can send (connection parameters, PHY, data length, MTU, security, unknown events) is answered or ignored without the firmware ever faulting, and if anything does fault the board resets within a second and records what happened, readable with one command (`?` → `rst=` and `last=`). This is bench-verified; the Garmin test is pending.

**All three metrics, and CSC versus CPS** (#1, #10, #15, #16, #17, #29, #35). Peloton derives speed from power, and the Cycling Power profile can carry wheel revolutions, so 0.2.0 sends power, cadence *and* speed/distance in one service (verified: Apple Watch and Wahoo show all of them). For devices that refuse a power meter the firmware can present itself as a speed/cadence sensor instead, or expose both services; the mode is a stored setting switched over Bluetooth, no reflashing. The wheel size to set on the client is 2105 mm (700x25c), adjustable with `W`.

**Power calibration** (#11, #36). The tablet applies a calibration factor the bus does not carry, so a calibrated bike shows one number on the screen and sends another. 0.2.0 has a stored scale and offset (`K`, `O`) applied before anything is transmitted; verified on the bike by comparing the transmitted power with the bike's own per-second export (×1.10 requested, ×1.108 measured).

**Firmware updates** (#3, #37). 0.2.0 has the same Buttonless DFU service as the stock firmware; the Android recipe from #37 (nRF Device Firmware Update app, 3000 ms reboot delay) is in the [DFU guide](dfu-guide.md). Stock units still need a package signed by the upstream developer ([developer-packaging.md](developer-packaging.md)); owners with a J-Link can install the open bootloader once ([flashing guide](flashing-guide.md)).

Everything else is a setup, hardware or app question answered from the existing user guide, or out of scope. Status values: **verified** (on our bike or bench, with what), **implemented, test pending** (built and bench-checked, needs the named device), **documented** (an answer or a guide, no firmware change), **out of scope** (with the reason).

## By issue

| # | Title | Upstream | What 0.2.0 does / answer | Status |
|---|---|---|---|---|
| #1 | Beta Firmware - Feedback Report | open | Speed is sent as wheel data inside Cycling Power; the Wahoo app shows it (18.2 mph live on 2026-10-15). Coros Pace 2: same profile, not tried | verified (Wahoo app); implemented, test pending (Coros) |
| #2 | Garmin Fenix 6 Connection Drops | closed | beta.4's clock (synthesized, 100 ppm) and connection parameters (20–1000 ms, 10 s supervision) reproduced; peer connection-parameter requests answered | implemented, Garmin test pending |
| #3 | Enabling Indications Failed | closed | DFU flow of the stock bootloader; the [DFU guide](dfu-guide.md) gives the iOS and Android steps that worked here (Android needs the reboot delay) | documented |
| #5 | Garmin Fenix 5 Plus dropped | closed | as #2; also the firmware advertises again immediately after any disconnect | implemented, Garmin test pending |
| #6 | Fenix 5s calorie data seems very low | closed | calories are computed by the watch from the power it receives; the firmware sends the bike's power (calibrated if you set `K`/`O`) | documented |
| #7 | Can't get it working | closed | cabling / ride mode question, see the upstream user guide; `?` over nRF Connect shows whether bus frames arrive (`ok=` counter) | documented |
| #8 | Compatible with Stryde bike? | closed | Peloton serial protocol only | out of scope (different bike) |
| #9 | Not working after recent Peloton update | closed | the decoder handles the 2020-era frames and the current ones (including the resistance-percent frame the current Peloton software sends); verified on an original Bike (replacement screen) running current Peloton software | verified (current software) |
| #10 | Is it still impossible to transmit speed, power and cadence at the same time? | closed | no: power + cadence + speed/distance in Cycling Power, plus FTMS Indoor Bike Data with resistance | verified (Apple Watch, Wahoo) |
| #11 | Request: add offset option | open | `K<scale×1000>` and `O<offset in 0.1 W>`, stored in flash, applied before transmission | verified (bike, 2026-10-15) |
| #12 | PeloMon as info to help get Speed | closed | yes: the protocol work was the starting point; credited in CREDITS.md. The speed curve itself was re-fitted from the author's rides | documented |
| #13 | Send data while not in ride mode | open | the tablet only polls the bike during a ride; a passive listener has nothing to listen to otherwise | out of scope (would need the DFC to poll the bike itself) |
| #14 | Unable to pair FR935 | closed | the board advertises as `DFC` whenever powered; check with nRF Connect first, then pair as a power sensor (mode 0) or speed/cadence sensor (mode 1) | documented; Garmin test pending |
| #15 | Won't pair with Garmin Venu 3 | closed | sensor mode 1 (`C1`, `B`): speed-and-cadence sensor, no power service, appearance 0x0485 | implemented, Garmin test pending (Venu 3) |
| #16 | Power + Cadence (maybe speed?) for Garmin | closed | mode 0 sends all three in Cycling Power | implemented, Garmin test pending |
| #17 | What size wheel to set (Garmin)? | open | 2105 mm (700x25c) on the client; the firmware's own value is adjustable with `W` so distance can be matched to the screen | verified (Apple Watch: 2.18 mi vs screen 2.17 mi) |
| #18 | Bike Setup | open | no firmware aspect; USB-A power for the DFC is what we use | documented |
| #19 | Power displaying 0 (Epix 2) | closed | as #2 / #24: the event path that reset the firmware cannot fault any more | implemented, Garmin test pending |
| #20 | Does it work with a Garmin FR965? | closed | pairs as a power sensor (mode 0); the robustness changes apply | implemented, Garmin test pending |
| #22 | Apple Watch | closed | the Watch pairs to the DFC directly (Health Devices) and records power, cadence, speed, distance; sending the Watch's heart rate to Zwift at the same time is a Watch limitation, not the DFC's | documented |
| #23 | DIY Display | closed | the decoder is a portable C module with host tests; an ESP32 port for non-DFC hardware is a planned follow-up | out of scope for 0.2.0 |
| #24 | Power display empty - Garmin Edge v21.09 | closed | the 2024 Garmin request that reset the firmware: every peer request is now handled or ignored, none can fault | implemented, Garmin test pending |
| #25 | Not transmitting to Garmin Epix 2 | closed | as #24 | implemented, Garmin test pending |
| #26 | Connected but no data | closed | as #24; `?` shows `rst=` (resets since power-up) and the bus counters to tell a radio problem from a cable problem | implemented, Garmin test pending |
| #27 | Fenix 6 Sapphire Does Not Detect | closed | as #2; appearance 0x0484 (Cycling Power Sensor) | implemented, Garmin test pending |
| #28 | Output not displaying on Peloton | closed | the firmware never drives the bus (bench-verified listener); a screen that loses data with the DFC in line points to the cable or the pass-through jack | documented |
| #29 | Speed/cadence/power for Fenix devices? | closed | mode 0 (all three in Cycling Power) or mode 2 (Cycling Power and Speed & Cadence both advertised) | implemented, Garmin test pending |
| #30 | No data on Peloton - solved | open | owner's hardware fix; nothing firmware-side | documented |
| #31 | Connected but no power/cadence in Zwift or Rouvy | open | Cycling Power and FTMS are both exposed; Zwift and Rouvy were not tested with this firmware | implemented, test pending (Zwift, Rouvy) |
| #32 | Pairing with Garmin Forerunner 965 | closed | as #20 | implemented, Garmin test pending |
| #33 | Garmin Epix 22.06 no data | open | as #24: a newer Garmin build sending another unexpected request cannot reset this firmware; `?` would show `rst=0` | implemented, Garmin test pending |
| #34 | Need help with MyWhoosh | open | FTMS Indoor Bike Data and Cycling Power are exposed; MyWhoosh was not tested | implemented, test pending (MyWhoosh) |
| #35 | Venu 4 power meter support | open | mode 0 (default) presents a Bluetooth power meter with cadence and speed | implemented, Garmin test pending (Venu 4) |
| #36 | Calibrated Peloton, output doesn't match Zwift | open | `K1250` makes the transmitted power match a screen that reads 25 % higher; modified firmware needs the open bootloader or a signed package, see the guides | verified (`K`, bike 2026-10-15); documented (signing) |
| #37 | Android DFU fix: reboot delay | open | adopted in the [DFU guide](dfu-guide.md) | documented |

## How to report

Connect with nRF Connect, open the UART tab, send `?`. The reply has the bus counters (`rx=`, `ok=`, `bad=`), the live values (`c=` cadence, `p=` power, `r=` resistance), `sd=` SoftDevice errors, `rst=` resets caused by faults and `last=<id>:<err>@<line>` naming the last one, and `fw=` the version. That line, plus the device you were connecting, is enough to diagnose almost anything.
