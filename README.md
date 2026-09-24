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

**Running on real hardware.** Flashed to a BRD2705A (xG28-EK2705A Explorer
Kit) via Simplicity Commander, boots, brings up the sub-GHz radio, advertises
over BLE as `ClaudeLinkSI` with the Insulin Pump Service UUID in the
advertisement itself (not just discoverable after connecting -- see "AAPS
couldn't see the device" below), accepts a connection, and exposes the real
GATT service (confirmed via `bluetoothctl`: `UUID: Vendor specific
(0235733b-99c5-4197-b856-69219c2a3845)`, the Insulin Pump Service). Nothing
has talked to an actual pump yet -- that's the next real milestone.

`src/` is wired into the actual Simplicity Studio project (`orangelink_xg28`'s
own `.slcp` source list) and `tools/build.sh` drives a full compile-and-link
through that project's own generated CMake/Ninja workflow, entirely from the
shell -- no Studio GUI needed. `tools/check_compile.sh` remains for a faster
syntax-only check of `src/` in isolation.

**A real bug was caught and fixed getting here**, worth keeping on record: on
first flash, the board hung completely silently -- no BLE advertisement, no
output, nothing. Bringing up RTT-based `printf()` logging (see "Diagnostic
logging" below) traced it to `sl_subg_radio_init()` calling
`sl_rail_util_init()`, which Silicon Labs' generated boot sequence
(`autogen/sl_event_handler.c`'s `sl_stack_init()`) already calls once,
automatically, before `app_init()` ever runs. The second call re-ran
`sl_rail_init()` against an already-initialized RAIL instance, which returned
`SL_STATUS_INVALID_PARAMETER` -- and since this project has no `app_log`
component, the resulting `app_assert()` failure path is a silent infinite
loop, not a printed error. Fixed in `sl_subg_radio.c`: just retrieve the
handle the framework already brought up, don't re-initialize it.

**AAPS couldn't see the device, even though `bluetoothctl` could.** The
board advertised and connected fine, and generic scanners (`bluetoothctl`,
nRF Connect) listed it by name -- but it never appeared in AndroidAPS's own
RileyLink device picker. AAPS's BLE scan filters on the advertised Insulin
Pump Service UUID (confirmed against this project's own RFM69 reference
firmware, which puts that same 128-bit UUID in the primary advertising
packet for exactly that reason), and this project's GATT Configurator had
the Insulin Pump Service marked `advertise="false"` -- inherited from
however the service was first created, never noticed because every scanner
used to verify hardware bring-up looks at the device by name/all-devices,
not by a service-UUID filter the way AAPS does. Fixed in
`app_bluetooth.c`'s `start_advertising()`: replaced the SDK's automatic
`sl_bt_legacy_advertiser_generate_data()` (whose "advertise" checkbox
behavior isn't represented anywhere in the generated `gatt_db.c` this
project's code can target) with an explicit advertising packet -- Flags +
the complete 128-bit Service UUID list -- and a scan response built from
the live Device Name characteristic (so a Custom Name rename still takes
effect on the next reconnect, as before). Verified against a from-scratch
`bluetoothctl` scan after fully removing the device from the host's BlueZ
cache: the Service UUID and correct advertising flags (`06`) appear before
any connection is made.

**AAPS connected but "problem connecting to pump": AndroidAPS's long GATT
writes weren't being reassembled.** `subg_rfspy` commands over ~20 bytes
(anything using CMD_SEND_AND_LISTEN with a real payload -- short commands
like CMD_GET_VER or CMD_UPDATE_REG were never affected) don't fit in one ATT
write at the default MTU, so Android's BLE stack splits them into a GATT
long write (Prepare Write + Execute Write) transparently to the app -- one
`writeCharacteristic()` call, two ATT operations underneath. IPS Data is
automatic (non-"user") GATT storage, so the stack delivers each queued
chunk as its own `sl_bt_evt_gatt_server_attribute_value_id` event with an
`offset` field describing the procedure; `start_advertising()`'s sibling
handler ignored both `att_opcode` and `offset` and dispatched every chunk
to `aps_put_cmd()` as if it were the whole command, so both fragments
failed the self-consistency check and were silently dropped. Confirmed
byte-for-byte against a real AndroidAPS log (`RFSpy.writeToDataRaw`, a
21-byte CMD_SEND_AND_LISTEND frame) cross-referenced with RTT output from
this firmware at the same moment. Fixed in `app_bluetooth.c`: Prepare Write
chunks are skipped (the stack already reassembles them into the attribute's
own backing store), and on Execute Write the full value is read back via
`sl_bt_gatt_server_read_attribute_value()` before dispatch. Verified: a
fresh AndroidAPS log after the fix shows CMD_UPDATE_REG writes getting real
Response Count notifications (01, 02, 03, ...) instead of "No response from
RileyLink".

**Bridge TX is confirmed; the bridge RX path is missing replies.** This is a Dynamic
Multiprotocol build, so TX supplies scheduler priority and bounded airtime,
then yields the shared radio. TX uses the actual encoded length plus a zero
terminator; the 107-byte value is the RX maximum. A controlled read-only AAPS
test set 4b6b, sent a 12-second wake burst, and swept 11 frequencies from
916.298 to 916.798 MHz in 50 kHz steps. The firmware read each requested
frequency back exactly. A simultaneous 90-second HackRF capture decoded 18
CRC-valid model replies from bench pump 646910 during the wake and scan window.
The bridge reported every request silent and did not deliver those replies to
the host. A separate passive listen at 916.650 MHz decoded a valid 560793 frame,
so the RX chain works on at least one channel; alignment or RAIL receive behavior
for the bench replies remains unresolved.

RX is explicitly configured for FIFO mode so the 1-byte threshold event used by
the zero-terminated reply handler can fire; packet mode had disabled that event
path. `sl_rail_start_rx()` supplies DMP scheduler priority 200 so BLE connection
events can share the radio. Do not interpret a foreign 560793 frame as a
646910 response. See [the hardware debugging handoff](DEBUGGING_NOTES_2026-09-23.md)
for the capture paths, per-frequency sweep results, and remaining diagnosis.

## Diagnostic logging

This project shipped with zero logging infrastructure. Added real RTT-based
`printf()` (SEGGER RTT over the debug probe -- no UART pins, no board-level
wiring) directly to the Simplicity Studio project, since it's what actually
caught the bug above:

- Components added to `orangelink_xg28.slcp`: `segger_rtt`, `iostream_rtt`,
  `printf`, `iostream_retarget_stdio`, `iostream_stdlib_config`, `iostream`,
  `cmsis_os2_ext_task_register`.
- Source copied from the SDK into the project (`segger/`, plus the relevant
  `platform_core/platform/service/iostream/*` and `printf/*` files) and wired
  into `cmake_gcc/orangelink_xg28.cmake` by hand, since no command-line SLC
  tool is available on this machine to regenerate the project from the
  `.slcp` outside Studio's own GUI (same caveat as the `src/` wiring below).
- `autogen/sl_event_handler.c` and two new `autogen/sl_iostream_*` files
  hand-adapted from a real generated reference (a sibling project using
  EUSART/VCOM instead of RTT) to wire the RTT instance into the boot
  sequence.

Read it with `commander rtt connect -d EFR32ZG28B312F1024IM48` while the
board is connected. Buffer content survives a flash or SWD reset -- only a
real power cycle (unplug/replug) clears it, which matters when comparing
"is this fresh?" against a new test. `app.c` has one permanent checkpoint
(the radio init result), `src/aps/aps.c`'s own `APS_LOG_*` macros are wired
to this same backend, and `sl_subg_radio.c`'s `sl_subg_send_pkt()` now logs
(first repeat only, since a real command can ask for 200+) if
`sl_rail_write_tx_fifo()`/`sl_rail_start_tx()` report anything but full
success, or if `SL_RAIL_EVENT_TX_PACKET_SENT` never arrives -- added while
chasing the radio-scheduling issue below, and worth keeping: a transmit
that silently never leaves the radio was otherwise indistinguishable from
one that sent fine and got no reply.

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

To flash a connected board, using Simplicity Commander:

```bash
commander flash cmake_gcc/build/base/orangelink_xg28.hex -d EFR32ZG28B312F1024IM48
```

(run from the Studio project's `cmake_gcc/` directory). First-time setup on
Linux needs Commander's udev rule installed (`sudo cp 99-jlink.rules
/etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm
trigger`, then replug the board) before it can see the on-board J-Link debug
probe.

## Remaining hardware checks

- **Bridge still doesn't receive bench pump 646910's replies; cause not yet
  found.** Ruled out: listen duration (a 60-second window still missed ~20
  real in-window replies), carrier mismatch as the sole cause (646910's true
  reply carrier was measured precisely at 916.6968 MHz from confirmed-reply
  timestamps, and tuning wake+listen within 3 kHz of it still caught zero of
  3 confirmed in-window replies), signal strength (646910's measured
  amplitude is comparable to or stronger than a different pump that *does*
  get through), and Dynamic Multiprotocol scheduler preemption (RX
  error-event logging stayed clean on every run). A frame-length hypothesis
  (long ~71-byte frames failing while short ones succeed) was raised and
  then retracted -- the test tool never actually distinguished short from
  long receptions in its output, so the evidence for it didn't hold up on
  review; the tool now prints frame length so this is checkable on the next
  test. See
  [the hardware debugging handoff](DEBUGGING_NOTES_2026-09-23.md) for the
  full analysis.
- Custom Name rename is RAM-only: no flash-backed settings storage exists
  yet, so it doesn't survive a power cycle the way "persist" implies in the
  legacy protocol.
- TX power control (`sl_subg_set_power_level`) is implemented but not
  hardware-verified.

## License

GPL-2.0-only, except the boot/advertising-setup portion of
`src/ble/app_bluetooth.c`, which is substantially unchanged from Silicon
Labs' Zlib-licensed example code and retains that license (see the file's
own header for exactly which part).
