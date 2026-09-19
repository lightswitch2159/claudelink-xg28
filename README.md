# Orangelink -- EFR32xG28 (native Simplicity SDK)

Status: **early scaffolding, not building yet.** No hardware exists for this
board on the bench. This is a starting skeleton written against real,
verified RAIL API signatures, not a working driver.

## What this is

A separate port from the Nordic `orangelink-ncs` repository, targeting the
Silicon Labs EFR32xG28 Explorer Kit (xG28-EK2705A / BRD2705A). It is
deliberately **not** a Zephyr/NCS project -- see
`orangelink-ncs/docs/BOARD-xg28-ek2705a.md` (that repo, `xg28-ek2705a`
branch) for the full reasoning: neither the BLE controller nor the sub-GHz
radio touch Zephyr's driver model on this chip regardless of which build
system wraps them, NCS does not support this vendor at all, and Silicon Labs
ships Dynamic Multiprotocol (BLE + custom RAIL PHY concurrently) as a
first-party combination on this exact die. This is a native Simplicity SDK
project, built and eventually generated through Simplicity Studio 6.

## What is real so far

- **The radio PHY is configured and saved in Studio**, built from the
  `RAIL - SoC RAILtest` example against the BRD2705A board target, protocol
  renamed `MDT_OOK`. Every field checked against the working RFM69 driver's
  own `rf69_cfg_916[]` register table (from `orangelink-ncs`,
  `feather-nrf52832` branch) or, where this chip's physics genuinely differs
  from the RFM69's (channel acquisition bandwidth), against the
  Configurator's own calculated value rather than a cross-chip guess:

  | | |
  |---|---|
  | Modulation | OOK, NRZ symbol encoding (not Manchester -- our own firmware already does Manchester/4b6b in software; letting the radio also Manchester-encode would double-encode) |
  | Bitrate | 16.384 kbps |
  | Frequency | 916 MHz base, 548 kHz channel spacing, channel 1 = 916.548 MHz |
  | Channel bandwidth | auto-calculated (350 kHz) -- a manually-forced 400 kHz (translated from the RFM69's own RxBw setting) produced a Configurator warning and was wrong |
  | Preamble | 128 bits, alternating |
  | Sync word | `0xFF00FF00`, 32 bits |
  | Frame length | FIXED_LENGTH, 107 bytes (matches `SUBG_MAX_PKT_LEN`, not the RFM69's own 255-byte hardware register ceiling, which is a different number for a different reason) |
  | Frame bit endian | MSB_FIRST |
  | CRC | off |

  Not yet exported from Studio into this tree -- the generated
  `autogen/`/config source Studio produces for this PHY has not been copied
  in here yet.

- **The encoding layer is ported verbatim.** `src/encoding/4b6b.{c,h}` and
  `manchester.{c,h}` are byte-identical copies from `orangelink-ncs`
  (`feather-nrf52832` branch) -- they had zero Zephyr dependency there and
  need no changes here.

- **A radio driver skeleton exists**, `src/drivers/rail/sl_subg_radio.{c,h}`,
  written against real `sl_rail_*()` signatures pulled from this session's
  already-fetched copy of `hal_silabs`
  (`modules/hal/silabs/simplicity_sdk/platform/radio/rail_lib/common/sl_rail.h`
  in the `orangelink-ncs-ws` workspace) rather than written from memory.
  **It has not been compiled or run.** Read the TODOs in both files before
  trusting anything in them -- several real gaps are marked rather than
  papered over:

  - The generated channel config symbol this driver needs to call
    `sl_rail_config_channels()` with does not exist in this tree yet (it is
    only inside the Studio project's own `autogen/`, not exported here).
    Without it, `sl_subg_radio_init()` never actually applies the PHY.
  - `sl_rail_set_rx_fifo()`'s exact parameter order was not verified this
    session (only its existence, by line-number search).
  - Blocking waits (`wait_for_rx_data_or_timeout()`, the TX-done wait) are
    written as placeholder busy-spins, explicitly marked as such. They need
    whatever real synchronization primitive the eventual RTOS/bare-metal
    environment provides -- not yet known, since it depends on which
    project template (bare RAILtest vs. an RTOS-based Bluetooth+DMP example)
    this becomes.
  - TX power control has no verified API name at all and is stubbed to fail.

- **Important correction already made once**: `RAIL_Init`/`RAIL_StartTx`/
  `RAIL_StartRx`/`RAIL_ConfigChannels` (PascalCase, "RAIL 2.x") are marked
  `@deprecated` in this SDK generation. The live API is the lowercase
  `sl_rail_*()` family ("RAIL 3"). This was caught before any code was
  written against the deprecated names, not after.

## What is not started

- BLE side entirely -- no work yet on Silicon Labs' native Bluetooth stack
  or the Dynamic Multiprotocol coexistence structure.
- The `aps.c`/`subg.c` protocol layer has not been ported. It is mostly
  hardware-independent policy logic in the RFM69 version and should port
  with modest changes once this driver's operations
  (`sl_subg_send_pkt`/`sl_subg_get_pkt`/RSSI) are real, but that has not been
  attempted.
- Nothing has run on real hardware. The board has not arrived yet.

## Provenance

Every RAIL fact in this README and in the driver skeleton's comments was
checked against real header/source files fetched into
`orangelink-ncs-ws/modules/hal/silabs` this session (pinned to Zephyr's own
`hal_silabs` revision, `f5201210afa1319ed8dd8dbe21682bcb63b25771` -- not
necessarily identical to whatever exact RAIL version Simplicity Studio
6 / SDK Suite 2026.6.1 itself ships, which is worth reconciling once the
real project is exported). Where something could not be grounded that way,
it is marked TODO rather than asserted.
