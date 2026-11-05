# Over-the-air updates and settings (nRF Connect)

For a DFC running this firmware (or the open bootloader from the [flashing guide](flashing-guide.md)), or a stock DFC with a package signed by the upstream developer. Updates take about a minute; bonds and settings are kept.

## Update on iOS

1. Install **nRF Connect for Mobile** from the App Store.
2. Put the update zip where the Files app can see it (iCloud Drive, AirDrop, Mail). Our packages are named `dfc_update_<version>.zip`.
3. In nRF Connect, connect to `DFC`, open the **DFU** tab, choose the zip, press **Start**. The board reboots into the bootloader, receives the image and reboots again; the app shows *Success*.
4. Reconnect and send `?` in the **UART** tab: the line ends with `fw=<version>`.

The bike cable can stay connected while the bike is idle (done twice on the author's bike without any effect on the tablet); unplugging it is the conservative choice.

## Update on Android

Use the **nRF Device Firmware Update** app (not the generic nRF Connect flow) and set the **reboot delay to 3000 ms** in its settings before starting; without the delay the app loses the board when it switches into the bootloader (upstream issue #37). Then select the zip, select `DFC`, upload.

## Settings over the UART tab

nRF Connect's **UART** tab (iOS: the tab with the terminal icon after connecting; it uses the Nordic UART Service) lets you send single-letter commands. Type the command, send, read the reply:

| Send | Effect |
|---|---|
| `?` | status line (`rx=… c=<rpm> p=<W> r=<%> … rst=<n> … fw=<version>`), then the settings line |
| `S` | settings line: `SET scale=<x1000> offset=<dW> wheel=<mm> csc=<mode> src=<default/fds/ram>` |
| `K1100` | power scale ×1.10 (`K1000` = none; range 500–2000) |
| `O-15` | power offset −1.5 W (deciwatts, −500…500) |
| `W2105` | wheel circumference in mm for the Cycling Power wheel data (500–3000) |
| `C1` | sensor mode: `C0` Cycling Power with wheel data (default), `C1` Speed and Cadence only (Garmin Venu 3), `C2` both; **takes effect after `B`** |
| `B` | clean reboot (applies a mode change) |
| `Z` | factory defaults (deletes the stored settings) |
| `T` | resistance table in use and whether it was read from your bike |

Settings are written to flash a moment after the command (`src=fds` on the next `S`). Full details, ranges and how calibration interacts with speed and distance: [peloton-protocol.md, Settings](peloton-protocol.md#settings) and [Sensor mode](peloton-protocol.md#sensor-mode-csc).

**Calibration:** if the bike's screen shows a different power than your watch after Peloton's calibration, set `K` to the ratio screen/watch × 1000 (for example `K1050` for +5 %) and, if needed, a small `O` offset. Power, the power-derived speed and distance all follow the calibrated value.

## Apple Watch

Pair under Settings → Bluetooth → Health Devices (or from the Workout app when it offers the sensor). Set the **wheel size to 2105 mm** (700x25c) there; the Watch computes speed and distance from the wheel data with that size. Start an Indoor Cycle workout; power, cadence, speed and distance appear live.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `DFC` not visible in nRF Connect | power-cycle the board, wait 15 s (slow advertising after boot); if two devices are already connected, disconnect one (the board takes two connections) |
| Watch or app connects but shows no data | start a ride on the Peloton (the bike only sends data in ride mode); check `?` shows `c=` and `p=` moving |
| Watch shows power but speed 0 | set the wheel size on the Watch (2105 mm) |
| `rst=` is not 0 | the firmware recorded a fault and recovered by resetting; note the `last=<id>:<err>@<line>` field and report it with what the connected device was |
| DFU fails on Android | reboot delay 3000 ms, see above |
| Mode change seems ignored | `C<n>` only stores the mode; send `B` (or power-cycle) |
