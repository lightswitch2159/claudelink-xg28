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
  written against real `RAIL_*()` signatures verified two different ways: by
  reading the real header on disk (`hal_silabs`, fetched into
  `orangelink-ncs-ws` this session) and by reading Simplicity Studio's own
  generated project on disk at
  `~/SimplicityStudio/v6_workspace/rail_soc_railtest/`. **It has not been
  compiled or run.** Two real, substantive corrections happened while writing
  it, both worth keeping on record rather than quietly fixing:

  1. **API family, corrected once already.** `RAIL_Init`/`RAIL_StartTx`/
     `RAIL_StartRx`/`RAIL_ConfigChannels` (PascalCase) are marked
     `@deprecated` in this SDK's header comments, in favour of a newer
     lowercase `sl_rail_*()` family. The driver was first written against
     `sl_rail_*` on that basis. That was wrong: Simplicity Studio's own
     Radio Configurator generates `RAIL_ChannelConfig_t` (the *old* type,
     named `MDT_OOK_channelConfig`, in `autogen/rail_config.c`) for this
     exact project, and its own generated init glue
     (`autogen/sl_rail_util_init.c`) calls `RAIL_Init`/`RAIL_ConfigData`/
     `RAIL_ConfigChannels` throughout -- not one `sl_rail_*` call anywhere in
     it. The deprecation tags are real, but this project template is built on
     the "deprecated" API regardless, and the two families are not
     interchangeable -- passing the generated `RAIL_ChannelConfig_t` to
     `sl_rail_config_channels()` (which takes `sl_rail_channel_config_t`)
     would be a straight type mismatch. Caught by reading the real generated
     file rather than trusting the header comments in isolation, but only
     after the first version was already written and briefly committed.

  2. **Bring-up should call the generated `sl_rail_util_init()`**
     (`autogen/sl_rail_util_init.h`), not reimplement
     `RAIL_Init`+`RAIL_ConfigChannels` by hand -- it is Silicon Labs' own
     tested glue, already wired to the real `channelConfigs[]`/
     `MDT_OOK_channelConfig`, including calibration and PA setup this driver
     has no reason to reimplement. `sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0)`
     retrieves the resulting handle afterward.

  Resolved since the last commit:

  - **The event callback wiring question is settled.** Read
    `autogen/sl_rail_util_callbacks.c` directly: `sl_rail_util_on_event()`
    is declared `__WEAK` there with an empty default body, and that file's
    own header warns "any application code placed within this file will be
    discarged upon project regeneration" -- the intended pattern is a
    weak-symbol override living outside `autogen/`, not editing the
    generated file or manually chaining callbacks. The driver's handler is
    now literally named `sl_rail_util_on_event()` with external linkage, so
    the linker uses it in place of the weak stub. No registration call
    needed. `RAIL_ConfigEvents()` in `sl_subg_radio_init()` is still
    required separately -- it controls which event bits are unmasked, not
    which function receives them, and those are genuinely two different
    questions.

  Real, still-open gaps, marked rather than papered over:

  - `RAIL_SetRxFifo()`'s exact parameter order was checked this time
    (`RAIL_Handle_t, uint8_t *addr, uint16_t *size`) -- confirmed, not
    assumed.
  - Blocking waits (`wait_for_rx_data_or_timeout()`, the TX-done wait) are
    written as placeholder busy-spins, explicitly marked as such. They need
    whatever real synchronization primitive the eventual RTOS/bare-metal
    environment provides -- not yet known, since it depends on which project
    template (bare RAILtest vs. an RTOS-based Bluetooth+DMP example) this
    becomes.
  - TX power control has no verified API name at all and is stubbed to fail.

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
checked against one of two real sources, not recalled from memory:

- header/source files fetched into `orangelink-ncs-ws/modules/hal/silabs`
  this session (pinned to Zephyr's own `hal_silabs` revision,
  `f5201210afa1319ed8dd8dbe21682bcb63b25771`), or
- Simplicity Studio's own generated project, live on this machine at
  `~/SimplicityStudio/v6_workspace/rail_soc_railtest/`, built against SDK
  Suite 2026.6.1.

The second source is what actually caught the RAIL_*/sl_rail_* mistake --
the header comments alone (from the first source) said the wrong thing
("use sl_rail_*, RAIL_* is deprecated") in a way that sounded authoritative
but did not match what Silicon Labs' own generator and generated init glue
actually do. Prefer checking the live generated project over the vendored
headers alone when the two could plausibly disagree. Where neither source
resolved something, it is marked TODO rather than asserted.
