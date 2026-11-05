# Packaging this firmware for stock DFC units (for the upstream developer)

Stock DFC units run the Nordic Secure DFU bootloader with signature checking, and only packages signed with your private key install on them. This firmware was built so that signing it is the only step between this repository and an over-the-air release to every DFC owner. This note gives the facts you need and the single command.

## What you get

| Item | Value |
|---|---|
| Artefact | `dfc_app_<version>.hex`, application only (built from `src/nordic`, also reproducible with `make -C src/nordic/armgcc -j`) |
| SDK / SoftDevice | nRF5 SDK 16.0.0, S140 7.0.1 (firmware id `0x00CA`), the SoftDevice already on the units |
| Application region | `0x27000`–`0xF5000` (linker `dfc_gcc_nrf52.ld`) |
| App data | `0xF5000`–`0xF8000`: FDS with the Peer Manager bonds and the owner settings; preserved by DFU (`NRF_DFU_APP_DATA_AREA_SIZE` 12288 on our bootloader; the stock bootloader keeps the same pages as long as the application region ends at `0xF5000`, which it does) |
| Bootloader | untouched: the package contains the application only; your bootloader at `0xF8000` and its settings page stay |
| Buttonless DFU | present (`ble_dfu_buttonless`, service `FE59`), so nRF Connect's DFU tab works as with your releases |
| Device name | `DFC`; Device Information reports manufacturer, model and firmware revision `<version>` |

## Sign and package

```
nrfutil nrf5sdk-tools pkg generate \
    --hw-version 52 \
    --sd-req 0x00CA \
    --application-version 6 \
    --application dfc_app_0.2.0.hex \
    --key-file <your private key .pem> \
    dfc_0.2.0_signed.zip
```

(`nrfutil pkg generate …` with the legacy nrfutil, same arguments.)

**Why `--application-version 6`.** The bootloader settings page of a unit running `power-cadence-beta.4` (read back from the author's unit before it was reflashed) holds `app_version = 5`. SDK 16's bootloader, with `NRF_DFU_APP_DOWNGRADE_PREVENTION` enabled (the example's default, `nrf_dfu_ver_validation.c`):

```c
else if ((p_init->fw_version > s_dfu_settings.app_version))  { return true; }
else if ((p_init->fw_version == s_dfu_settings.app_version)) { return NRF_DFU_APP_ACCEPT_SAME_VERSION; }
else                                                         { return false; }
```

so the package must carry a version above the one installed. If your bootloader was built without downgrade prevention the value does not matter; 6 is safe either way. Units still on v1.5 have a lower `app_version` and accept it too.

## What survives the update

Bonds and the owner settings (power calibration, wheel size, sensor mode) live in the FDS pages and are kept. The first install from your firmware creates the settings record on first use; defaults apply until then.

## What to test on Garmin hardware

We have no Garmin device. The firmware reproduces your beta.4's connection settings (LFCLK synthesized at 100 ppm, PPCP 20–1000 ms / 10 s supervision), answers every peer request (connection parameters, PHY, data length, MTU, security parameters, unknown events) without ever reaching `APP_ERROR_CHECK`, and records any fault in a reset-surviving trace instead of halting. Two differences from beta.4 are deliberate: the Fitness Machine service is advertised next to Cycling Power, and the appearance is `0x0484` (Cycling Power Sensor) instead of `0x0480`.

With nRF Connect's UART tab (Nordic UART Service):

| Test | Device | Commands | Pass when |
|---|---|---|---|
| Power meter + wheel speed (default mode 0) | Venu 4 (#35), Fenix 6 (#2, #5, #27), Edge 21.09+ (#24), Epix (#25, #33), FR 965 (#20, #32) | none; pair as a power sensor; optionally also as a speed sensor | power, cadence and speed shown; `?` keeps `rst=0` across a 10-minute ride |
| Speed and cadence only (mode 1) | Venu 3 (#15) and any unit that refuses a power meter | `C1` then `B` | pairs as a speed/cadence sensor, no Cycling Power service offered; `rst=0` |
| Both services (mode 2) | any | `C2` then `B` | the unit picks one and shows data; note which |
| Back to default | | `C0` then `B` | |
| FTMS present | any Garmin | none | the Fitness Machine service does not prevent pairing (if it does, tell us: it can be gated by the same mode setting) |

If a unit misbehaves, send `?` afterwards: `rst=<n>` counts resets caused by faults and `last=<id>:<err>@<line>` names the SoftDevice call, the error and the source line. That line is all we need to fix it.

## Building it yourself

```
make -C src/nordic/armgcc -j            # application hex in src/nordic/armgcc/_build
make -C src/nordic/armgcc dfu_pkg       # unsigned zip (our open bootloader only)
```

The Makefile's `dfu_pkg_signed` target shows the same nrfutil invocation with our development key; substitute yours. Host tests for the decoder: see the README.

## Source

Public branch `release` of this fork: firmware, decoder tests, bench and fit scripts, the protocol document (`docs/peloton-protocol.md`) with the bench verification history, and the owner guides. The firmware source is Apache-2.0 and contains no third-party licensed data (the speed curve is fitted from the author's own rides, the resistance table was read from the author's bike).
