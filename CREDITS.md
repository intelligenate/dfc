# Credits


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
