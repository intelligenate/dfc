# Flashing guide: one-time SWD install of the open bootloader

This is the path for owners who do not have a signed package from the upstream developer. You flash the board once over its debug port with a J-Link; from then on every update is over the air with nRF Connect ([DFU guide](dfu-guide.md)). The procedure below is the one used on the author's board on 2026-10-14; the numbers in it (SWD clock, the triple FICR read) come from real failures on this board's connector, not from caution.

## What this does to your DFC

The stock DFC bootloader only installs packages signed by the upstream developer. This procedure erases the chip and installs the **open bootloader** (same Nordic Secure DFU bootloader, signature check disabled) together with this firmware. Afterwards:

- any DFU package built from this repository installs over the air, and so does any package the upstream developer publishes (signed or not);
- the stock bootloader is gone. A signed upstream package cannot bring it back; only another SWD flash can. If you want the stock state again, keep the readback described below.

## What you need

| Item | Notes |
|---|---|
| SEGGER J-Link (EDU Mini is enough) | with `nrfjprog` and `mergehex` installed (Nordic command-line tools) |
| Tag-Connect TC2030-IDC-NL cable | the DFC has the 6-pin TC2030 footprint; wiring and adapter details in [jlink-swd-cable-wiring.md](jlink-swd-cable-wiring.md) |
| USB-A power for the DFC | power the board from a USB-A port or charger, not from the tablet's micro-USB |
| This repository built | `make -C src/nordic/armgcc merge` produces `_build/dfc_merged.hex` (SoftDevice + bootloader + settings + application); building the bootloader once is described in [src/nordic/bootloader/README.md](../src/nordic/bootloader/README.md) |

## Rules

1. **The bike cable must be unplugged** while the J-Link is attached. When the debugger halts or resets the chip, the pins that control the RS-232 transceiver are no longer driven low, the transceiver starts driving the bike's bus and the tablet freezes until the bike is power-cycled (seen on the bench). Unplug the bike, flash, unplug the J-Link, then reconnect the bike.
2. **Power from USB-A** during the whole procedure.
3. **Run SWD at 1 MHz.** On this connector the link dropped single bits at the default 4 MHz; a corrupted write to the UICR can lock the chip (APPROTECT). Every command below passes `--clockspeed 1000`; the Makefile does the same (`SWD_KHZ`).

## Steps

1. Attach the Tag-Connect cable to the footprint, power the board from USB-A, plug in the J-Link.
2. Check the link by reading the chip's factory page three times and comparing:
   ```
   for i in 1 2 3; do nrfjprog -f nrf52 --clockspeed 1000 --memrd 0x10000000 --n 1024 > ficr_$i.txt; done
   diff ficr_1.txt ficr_2.txt && diff ficr_2.txt ficr_3.txt && echo "link clean"
   ```
   If the three reads differ, reseat the cable and repeat. Do not flash on a link that drops bits.
3. Optional but recommended: keep a readback of the stock firmware.
   ```
   nrfjprog -f nrf52 --clockspeed 1000 --readcode stock_readback.hex
   nrfjprog -f nrf52 --clockspeed 1000 --readuicr stock_uicr.hex
   ```
4. Flash. From the repository root:
   ```
   make -C src/nordic/armgcc flash
   ```
   which runs `nrfjprog --program _build/dfc_merged.hex --chiperase --verify`, restores the reset pin configuration in the UICR (`PSELRESET` = P0.18, two writes of `0x12`), and resets the chip.
5. Unplug the J-Link, then plug the bike cable back in.

## Check

- The board advertises as `DFC` within seconds. nRF Connect shows the Cycling Power, Fitness Machine, Device Information, Battery, Nordic UART and Secure DFU services.
- In the UART tab send `?`: the reply ends with `dfu=1 ram=20003F10 fw=<version>`. `rst=0` means no fault has been recorded since power-up.
- Your watch or app must be paired again (the chip erase removed the old bond).

## Recovery

- **Erase fails (J-Link error -102) on a board that keeps resetting:** `nrfjprog -f nrf52 --clockspeed 1000 --recover`, then flash again.
- **Chip reports APPROTECT / readback protection:** `--recover` clears it (and erases everything); then flash.
- **Board not found by nRF Connect after flashing:** power-cycle it; the first advertising interval after boot is slow, give it 15 s.
- **Back to stock:** flash `stock_readback.hex` with `--chiperase` and restore the UICR from `stock_uicr.hex` (`--program stock_uicr.hex`), both at 1 MHz.
