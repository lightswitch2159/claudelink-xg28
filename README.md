# Orangelink -- EFR32xG28 (native Simplicity SDK)

Status: **compiles clean, not run yet.** No hardware exists for this board on
the bench, so nothing has executed -- but every file in `src/` now passes a
real `-fsyntax-only` compile against the exact toolchain, include paths, and
preprocessor defines Simplicity Studio's own generated project uses for this
board (`tools/check_compile.sh`, reproducible, extracts its flags directly
from the live generated project rather than hand-maintaining a copy). Zero
errors, zero warnings with `-Wall -Wextra`. That is a real, if partial,
verification -- every header include resolves, every `RAIL_*()` call site
matches its actual declared signature, every constant used is real -- not
just a plausible-looking skeleton.

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
  `~/SimplicityStudio/v6_workspace/rail_soc_railtest/`. **Verified to compile
  clean** against that project's exact real toolchain/include/define set
  (`tools/check_compile.sh`) -- not run on hardware, but a real, non-trivial
  check, not just a plausible-looking skeleton. Two real, substantive
  corrections happened while writing it, both worth keeping on record rather
  than quietly fixing:

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
  - **RTOS question resolved: FreeRTOS.** Silicon Labs ships exactly two DMP
    (Bluetooth + proprietary RAIL) project templates that actually coexist
    BLE with a RAIL protocol -- `bt_rail_dmp_soc_empty` and
    `bt_rail_dmp_soc_light` -- and *both* require an RTOS: each ships a
    FreeRTOS variant and a Micrium OS variant, with no bare-metal DMP option
    at all. Confirmed by reading the real `.slcp` manifests and example
    source under
    `~/.silabs/slt/installs/conan/p/simpleca33d691c539/p/bluetooth_le_app/example/bt_rail_dmp_soc_empty/`,
    not inferred from documentation. FreeRTOS chosen over Micrium OS as the
    more portable/open option. The two former busy-spin placeholders
    (RX-data-or-timeout, TX-done) are now a real FreeRTOS event group,
    grounded against the real `event_groups.h`/`task.h` on disk (bits set
    from `sl_rail_util_on_event()` via the ISR-safe `...FromISR()` API --
    the DMP example's own `readme.md` states that callback runs from
    interrupt context, so this is not a guess). While in there, fixed a real
    latent bug the busy-spin version had: a full 107-byte receive with no
    `0x00` terminator never set the "have data" flag, since only the
    terminator path did.
  - **CAUTION, the API family question may not be settled after all.** That
    same DMP example's `freertos/app_proprietary.c` defines a *live*
    (non-commented, must compile) `sl_rail_util_on_event(sl_rail_handle_t,
    sl_rail_events_t)` -- the lowercase family, contradicting this driver's
    `RAIL_*` choice above. Traced to a real, specific cause rather than left
    as a contradiction: `rail_soc_railtest.slcp` pulls in SDK component id
    `rail_util_init` (legacy, confirmed generating `RAIL_*` glue from its own
    autogen output), while `bt_rail_dmp_soc_empty_freertos.slcp` pulls in a
    *differently-named* component, `sl_rail_util_init` -- two distinct SDK
    components, not one component behaving two ways. This driver's `RAIL_*`
    calls are still correct for the currently-generated `rail_soc_railtest`
    project they're checked against, but will likely need rewriting to
    `sl_rail_*` once a real `bt_rail_dmp_soc_empty_freertos` project is
    generated for BRD2705A and its own `autogen/` output can be read -- same
    discipline as the original correction, not resolved by this note alone.
  - TX power control now calls a real API, `RAIL_SetTxPowerDbm()` (confirmed
    from the real `rail.h` this session -- deci-dBm units, not the RFM69
    driver's raw PA register value). Not hardware-verified, and depends on
    `RAIL_ConfigTxPower()` having already run during `sl_rail_util_init()`,
    which is expected but not independently confirmed for this exact project.
  - Frequency control (`sl_subg_set_freq`/`sl_subg_get_freq`) was added for
    the protocol layer below to call. RAIL has no arbitrary-Hz tune, only
    channel selection, so these map a requested Hz onto the nearest channel of
    the static channel config (`SL_SUBG_BASE_FREQ_HZ` + n \*
    `SL_SUBG_CHANNEL_SPACING_HZ`, channels 0-20) rather than reconfiguring the
    PHY.

- **The APS protocol layer is ported.** `src/aps/aps.{c,h}` -- the RileyLink
  command handler (`subg_rfspy 2.2`: `CMD_GET_STATE`, `CMD_SEND_PKT`,
  `CMD_SEND_AND_LISTEN`, register read/write, statistics, etc.) -- from
  `orangelink-ncs:feather-nrf52832`. Command parsing, the CC111x-style
  register encoding, the overflow bounds check, and the deferred-frequency-
  write reasoning are carried over unchanged; none of it is RFM69-specific.
  What did change, in full in `aps.c`'s file banner:

  - Radio calls go through `sl_subg_*()` instead of `rf69_*()`/`subg_*()`.
  - `ips_send_response()` (BLE-specific) is replaced by `aps_transport_send()`
    (new `src/aps/aps_transport.h`/`.c`), a narrow interface with a weak
    no-op default -- the same weak-symbol-override pattern already used for
    `sl_rail_util_on_event()`, one layer up, so this compiles and its command
    parsing is testable before a BLE layer exists.
  - Zephyr's thread+message-queue dispatch, byte-order helpers, and logging
    macros are gone (this is not a Zephyr build). **Dispatch now runs on a
    real FreeRTOS task and static queue** -- the direct structural
    equivalent of the source's `k_thread`/`k_msgq` (depth 4, matching), now
    that FreeRTOS is confirmed as the sanctioned RTOS (see the driver's RTOS
    note above). An earlier version of this port dispatched synchronously and
    inline instead, while the RTOS question was still open; that version is
    gone. `aps_put_cmd()` now enqueues (non-blocking, matching the source's
    `K_NO_WAIT`) and returns immediately, so whatever calls it -- the BLE GATT
    write callback, once that layer exists -- is not blocked for the
    duration of a long listen, restoring the exact property the interim
    synchronous version gave up. The dispatch task's priority
    (`APS_TASK_PRIORITY`) is a placeholder, not yet checked against a real
    DMP project's Bluetooth task priorities the way the source's own comment
    says it must sit below.

  Verified to compile clean, zero warnings, alongside the driver
  (`tools/check_compile.sh` now covers `src/aps/*.c` too, including the
  FreeRTOS headers both files now depend on -- see the script's own comment
  for the weaker-verification-tier caveat: there is no real generated
  FreeRTOS/DMP project on disk yet, so this checks against the raw SDK
  headers rather than a project's own generated build flags).

## What is not started

- BLE side entirely -- no work yet on Silicon Labs' native Bluetooth stack
  or the Dynamic Multiprotocol coexistence structure. `aps_transport_send()`
  has no real implementation to hand responses to yet.
- **Generating a real `bt_rail_dmp_soc_empty_freertos` project for BRD2705A
  in Simplicity Studio** is the next concrete step -- it would settle the
  RAIL_*/sl_rail_* question for real (see above), give real Bluetooth task
  priorities to tune `APS_TASK_PRIORITY` against, and give a real
  `cmake_gcc/*.cmake` to extract `tools/check_compile.sh`'s FreeRTOS flags
  from instead of the current borrowed/coincidental set. Needs Simplicity
  Studio's GUI, same as the RAIL PHY configuration was done.
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
