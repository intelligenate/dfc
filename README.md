# DFC - Data Fitness Connector

Free your data! 

DFC allows the Peloton Bike to communicate with fitness watches, head units, and apps — a feature that’s not available on the stock Bikes. It reads power and cadence data in real-time, through a cable connected to your bike, and broadcasts them wirelessly to nearby devices including those built around platforms like Zwift and Garmin. This lets you enjoy the built-in functionality of your bike while simultaneously utilizing features and services that depend on third-party platforms.

* Supports two simultaneous wireless connections for connecting to both a wearable and app at the same time
* Wireless device firmware updates for future upgrades
* Open and expandable platform for adding new features down the line

This repository is the home to the open design files for DFC. 

DFC can be ordered from the [Crowd Supply page here](https://www.crowdsupply.com/intelligenate/data-fitness-connector) and more information on DFC can be found on [our website](https://intelligenate.com/dfc.html).

See the [User Guide](https://github.com/intelligenate/dfc/blob/main/docs/user-guide.md) for details on how to setup and use DFC.

# Open firmware for the DFC (this fork)

This fork carries an independent, unofficial firmware for the same DFC board, rebuilt from scratch on the Nordic nRF5 SDK 16 with the SoftDevice the stock units already run. It listens to the Peloton bike's serial bus passively (the bike and its screen never notice it) and broadcasts everything the bus carries over standard Bluetooth profiles:

| Service | Data | Who uses it |
|---|---|---|
| Cycling Power (CPS) | power, cadence (crank data), speed and distance (wheel data) | Apple Watch, Wahoo, Zwift, Garmin units that accept a power meter |
| Fitness Machine (FTMS) Indoor Bike Data | speed, cadence, resistance 0–100 %, power | apps that read trainers |
| Cycling Speed and Cadence (CSC), optional | speed and cadence | Garmin units that refuse a power meter (Venu 3) |

One firmware serves three sensor shapes (CPS with wheel data, CSC only, or both), selected by a setting that survives resets and updates. Two devices can connect at the same time (a watch and an app). Updates are over the air with nRF Connect. Power calibration (scale and offset), wheel size and the sensor mode are set over Bluetooth, no tools beyond nRF Connect.

**Verified** on an original Peloton Bike (replacement touchscreen, current Peloton software): Apple Watch 110 W / 75 rpm / 2.18 mi against the bike's own summary of 111 W / 76 rpm / 2.17 mi over an 8-minute ride, with the Wahoo app connected as the second device. Garmin devices are not verified by us: the firmware reproduces the connection settings of the upstream `power-cadence-beta.4` release and never faults on an unexpected peer request, but the Garmin tests are pending with the upstream developer.

## Install

Stock DFC units only accept update packages signed by the upstream developer, so there are two paths:

1. **Signed package from the developer** (no tools): if a signed release of this firmware is published upstream, install it like any DFC update ([DFU guide](docs/dfu-guide.md)).
2. **One-time SWD flash** (J-Link and a Tag-Connect cable): install the open bootloader and this firmware yourself, after which any build updates over the air ([flashing guide](docs/flashing-guide.md), then the [DFU guide](docs/dfu-guide.md)). This replaces the stock bootloader; see the guide for what that means.

## Settings and sensor mode

Connect with nRF Connect, open the UART tab and send single-letter commands: `?` shows the status line, `S` the settings, `K` / `O` set the power scale and offset, `W` the wheel circumference, `C` the sensor mode (then `B` to reboot into it), `Z` restores defaults. The full table with ranges is in the [protocol document](docs/peloton-protocol.md#settings); the [DFU guide](docs/dfu-guide.md) walks through it. Apple Watch: set the wheel size to 2105 mm (700x25c) in the Health Devices settings for correct speed and distance.

## Build from source

Toolchain: `arm-none-eabi-gcc`, the nRF5 SDK 16.0.0 unpacked under `nrf5_sdk/`, `nrfutil` with the `nrf5sdk-tools` command for packages, `nrfjprog` for SWD. Then:

```
make -C src/nordic/armgcc -j          # application hex
make -C src/nordic/armgcc dfu_pkg     # OTA zip for the open bootloader
cd src/nordic/tests && cc -std=c11 -Wall -Wextra -I.. -o /tmp/test_peloton test_peloton.c ../peloton.c -lm && /tmp/test_peloton
```

Upstream issues this firmware addresses, by number: [docs/upstream-issues.md](docs/upstream-issues.md).

The decoder (`src/nordic/peloton.c`) has no SDK dependencies and is covered by host tests with captured bus frames. `docs/peloton-protocol.md` documents the bus protocol, the Bluetooth payloads, the diagnostic channel and what was verified when. `docs/developer-packaging.md` tells the upstream developer how to sign this build for stock units.

## Licence and credits

The firmware source is Apache-2.0 like the rest of this repository. Since 0.2.0 it contains no third-party licensed data: the speed curve is fitted from the author's own rides and the default resistance table was read from the author's bike. The Peloton protocol work by Imran Haque (PeloMon) made this possible and is credited in [CREDITS.md](CREDITS.md). This fork is not affiliated with Intelligenate or Peloton.

# Notice

DFC is a product of Intelligenate, LLC. DFC is not affiliated with Peloton Interactive, Zwift, Garmin or any other company mentioned in this repository. All trademarks are the property of their respective owners.