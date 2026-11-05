# J-Link EDU Mini — SWD Cable Wiring

**Verified working:** 2026-07-01, connected to DFC board (nRF52840), read core + VTref successfully.

This documents the SWD debug cable configuration for connecting a **SEGGER J-Link EDU Mini V2** to a target via jumper wires and an Adafruit adapter. Reusable for any 3.3V ARM Cortex-M target, not just the DFC.

---

## Hardware chain

```
J-Link EDU Mini V2
  └─ 2x5 1.27mm ribbon ─→ Adafruit #2094 (2x5 1.27mm ⇄ 2x10 0.1" ARM-20)
                            └─ jumper wires ─→ TC2030-IDC-NL cable
                                                 └─ 6-pin Tag-Connect ─→ target board
```

- **J-Link EDU Mini V2** (Adafruit #3571) — 10-pin (2x5, 1.27mm) Cortex-M connector.
- **Adafruit #2094** — passive adapter: 2x5 1.27mm SWD side ⇄ 2x10 0.1" (2.54mm) ARM-20 header. Jumpers go on the 0.1" ARM-20 side.
- **Tag-Connect TC2030-IDC-NL** — 6-pin Tag-Connect (no-legs) cable to the target footprint.

---

## TC2030 pinout (standard ARM Cortex 6-pin)

```
Pin 1: 3.3V / VTref
Pin 2: SWDIO
Pin 3: nRESET
Pin 4: SWCLK
Pin 5: GND
Pin 6: SWO (or NC)
```

> Note: an earlier project handoff claimed the DFC footprint was non-standard (pin 1 = RESET,
> pin 3 = 3.3V). Live testing on 2026-07-01 **disproved that** — VTref read 3.341V with pin 1
> wired as 3.3V, and the core was identified normally. Pin 1 = 3.3V is correct/standard.

---

## Adafruit #2094 — 2x10 ARM-20 header (0.1" / 2.54mm)

```
 1  VTref        2  Vsupply(NC)
 3  nTRST        4  GND
 5  TDI          6  GND
 7  SWDIO/TMS    8  GND
 9  SWCLK/TCK   10  GND
11  RTCK        12  GND
13  SWO/TDO     14  GND
15  nRESET      16  GND
17  NC          18  GND
19  NC          20  GND
```

---

## Jumper mapping (the 5 wires you need)

| Signal        | TC2030 pin | Adafruit #2094 (ARM-20) pin |
|---------------|:----------:|:---------------------------:|
| 3.3V / VTref  | 1          | **1**                       |
| SWDIO         | 2          | **7**                       |
| nRESET        | 3          | **15**                      |
| SWCLK         | 4          | **9**                       |
| GND           | 5          | **4** (or any even 4–20)    |
| SWO (optional)| 6          | **13**                      |

---

## Power — important

The **J-Link EDU Mini does NOT supply power to the target.**

- The `power on` command (5V on pin 19) exists only on full-size J-Links (BASE/PLUS/ULTRA+/PRO).
- On the EDU Mini, **pin 1 (VTref) is sense-only** — it reads target voltage, it does not drive it.
  Do **not** back-feed power into VTref.
- The target must have its own power source (bench supply on the 3.3V rail, USB, or its normal
  supply). The J-Link senses that voltage via VTref and talks SWD.

---

## Verification steps

1. **Probe alive on USB:**
   ```
   printf 'ShowEmuList\nexit\n' | JLinkExe -nogui 1
   ```
   Expect: firmware banner, S/N, `License(s): FlashBP, GDB`.

2. **Target connection (target must be externally powered):**
   ```
   printf 'connect\nnRF52840_xxAA\nS\n4000\nexit\n' | JLinkExe -nogui 1
   ```
   Expect:
   - `VTref=3.3xxV` (confirms pin 1 / power wiring)
   - `Found SW-DP with ID 0x2BA01477`
   - `Found Cortex-M4 ... nRF52840` core identified

   Swap `nRF52840_xxAA` for the target device on other projects.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `VTref=0.000V` | Target not powered, or pin 1 (3.3V/VTref) jumper not connected |
| `Failed to open DLL` / probe not on USB | USB cable/link dropped — reseat cable, try another port |
| SW-DP not found | SWDIO/SWCLK/GND jumper wrong, or target held in reset |
| Connects but flaky | Lower SWD speed (e.g. 1000 kHz), check jumper/ground integrity |

---

*Created 2026-07-01. Reference: SEGGER J-Link Commander V9.16a, J-Link EDU Mini V2 (S/N 802006821).*
