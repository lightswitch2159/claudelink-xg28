# Orangelink -- EFR32xG28

A RileyLink-compatible sub-GHz↔BLE bridge for Medtronic 916 MHz pumps,
targeting the Silicon Labs EFR32xG28 Explorer Kit (xG28-EK2705A / BRD2705A).
Native Simplicity SDK project (Dynamic Multiprotocol: Bluetooth + a
proprietary RAIL PHY, concurrently, on one radio core), built through
Simplicity Studio 6.

Protocol logic (the RileyLink `subg_rfspy` command set) and the 4b6b/Manchester
line-coding layer are ported from this project's own RFM69-based reference
firmware.

## Status

**Builds and links clean into a real firmware image, not run on hardware
yet** -- no board on the bench. `src/` is wired into the actual Simplicity
Studio project (`orangelink_xg28`'s own `.slcp` source list) and
`tools/build.sh` drives a full compile-and-link through that project's own
generated CMake/Ninja workflow, entirely from the shell -- no Studio GUI
needed. Produces a real `orangelink_xg28.{out,hex,bin}` (250 KB code, 1 KB
data, 300 KB bss). `tools/check_compile.sh` remains for a faster
syntax-only check of `src/` in isolation.

## Architecture

```
src/
  encoding/     4b6b.c, manchester.c -- line coding, selected per-command by the host
  drivers/rail/ sl_subg_radio.{c,h}  -- RAIL driver: init, TX/RX, RSSI, power, frequency
  aps/          aps.{c,h}            -- RileyLink command dispatch (subg_rfspy 2.2)
                aps_transport.{c,h}  -- weak transport interface aps.c sends responses through
  ble/          app_bluetooth.c      -- BLE event handling + aps_transport_send() implementation
  app.c                              -- application entry point (sl_subg_radio_init + aps_init)
```

**Radio**: `sl_rail_*()` (not the deprecated-looking-but-actually-current
`RAIL_*` family some header comments suggest -- this project's own generated
init glue uses `sl_rail_*` throughout). RX/TX driven by a Micrium OS event
group (`OSFlagPend`/`OSFlagPost`), set from the RAIL event callback. PHY:
OOK, 16.384 kbps, 916 MHz base / 548 kHz channel spacing (channel 1 =
916.548 MHz), 107-byte fixed-length frames, CRC off -- configured in the
project's own Radio Configurator, confirmed against the real saved
`config/rail/radio_settings.radioconf`.

**Protocol dispatch**: a dedicated Micrium OS task reading a depth-4 `OS_Q`,
mirroring the legacy firmware's own dedicated-thread design so a multi-second
`CMD_SEND_AND_LISTEN` doesn't stall the BLE stack. `aps_put_cmd()` enqueues
non-blocking and returns immediately.

**BLE GATT layout** -- reproduces the legacy RileyLink "Insulin Pump Service"
exactly, so AndroidAPS/Loop see the same surface they already know how to
talk to:

| Characteristic | UUID | Properties | Length |
|---|---|---|---|
| Data | `C842E849-5028-42E2-867C-016ADADA9155` | Read, Write | ≤150, variable |
| Response Count | `6E6C7910-B89E-43A5-A0FE-50C5E2B81F4A` | Read, Notify | 1 |
| Timer Tick | `6E6C7910-B89E-43A5-78AF-50C5E2B86F7E` | Read, Notify | 1 |
| Custom Name | `D93B2AF0-1E28-11E4-8C21-0800200C9A66` | Read, Write | ≤30, variable |
| Version | `30D99DC9-7C91-4295-A051-0A104D238CF2` | Read | 13, fixed `"ble_rfspy 2.0"` |
| LED Mode | `C6D84241-F1A7-4F9C-A25F-FCE16732F14E` | Read, Write | 1 |

Service UUID `0235733B-99C5-4197-B856-69219C2A3845`. Device name
`ClaudeLinkSI`. A GATT write to Data calls `aps_put_cmd()`;
`aps_transport_send()` writes the response back to Data, then increments and
notifies Response Count -- that ordering is mandatory, the client only reads
Data after seeing the notification.

## Building

Requires the Simplicity Studio project `orangelink_xg28`
(`~/SimplicityStudio/v6_workspace/orangelink_xg28/`), generated from Silicon
Labs' "Bluetooth RAIL DMP - SoC Empty Micrium OS" example for BRD2705A, with
this repo's `src/aps`, `src/drivers/rail`, and `src/encoding` copied into the
project (as `aps/`, `drivers/rail/`, `encoding/`) and added to its own
`orangelink_xg28.slcp` `source:`/`include:` lists -- `src/app.c` and
`src/ble/app_bluetooth.c` replace the project's generated `app.c`/
`app_bluetooth.c` directly (already declared in the `.slcp`, no edit needed
for those two).

```bash
tools/build.sh            # full compile + link, produces a real firmware image
tools/check_compile.sh    # faster: syntax-checks src/ in isolation, no link
```

## Not started

- No hardware validation -- board hasn't arrived. Nothing has been flashed.
- Custom Name rename is RAM-only: no flash-backed settings storage exists
  yet, so it doesn't survive a power cycle the way "persist" implies in the
  legacy protocol.
- TX power control (`sl_subg_set_power_level`) and frequency retuning
  (`sl_subg_set_freq`) are implemented but not hardware-verified.

## License

GPL-2.0-only, except the boot/advertising-setup portion of
`src/ble/app_bluetooth.c`, which is substantially unchanged from Silicon
Labs' Zlib-licensed example code and retains that license (see the file's
own header for exactly which part).
