# Orangelink -- EFR32xG28 (native Simplicity SDK)

Status: **compiles clean, not run yet.** No hardware exists for this board on
the bench, so nothing has executed -- but every file in `src/` now passes a
real `-fsyntax-only` compile against the exact toolchain, include paths, and
preprocessor defines Simplicity Studio's own generated project uses for this
board (`tools/check_compile.sh`, reproducible, extracts its flags directly
from the live generated project rather than hand-maintaining a copy). Zero
errors, zero warnings with `-Wall -Wextra`. As of this check, that project is
`orangelink_xg28` -- imported directly from the real
"Bluetooth RAIL DMP - SoC Empty Micrium OS" example (`bt_rail_dmp_soc_empty`,
package `bluetooth_le_app`), a clean minimal Bluetooth+RAIL DMP skeleton with
none of the Range Test/CLI/LCD baggage the previous reference project
(`rail_bt_dmp_soc_range_test`) carried. It verifies the actual `sl_rail_*()`
API family and Micrium OS integration this port needs, not just RAIL in
isolation, and now includes a real custom BLE GATT service (see below) on top
of that. Every header include resolves, every call site matches its actual
declared signature, every constant used is real -- not just a
plausible-looking skeleton.

**Project history, since it's not obvious from the file layout:** three
Simplicity Studio projects exist on disk. `rail_soc_railtest` (bare RAIL, no
Bluetooth/RTOS) was the original reference that got the encoding layer and
first driver skeleton compiling, and resolved the `RAIL_*` (not `sl_rail_*`)
API family question *for that project shape*. `rail_bt_dmp_soc_range_test`
(a real Bluetooth+RAIL DMP example, Micrium OS) replaced it as the primary
target once BLE work started, and is what caught the *actual* `sl_rail_*`
family and Micrium OS RTOS this port needs. It was then itself superseded by
`orangelink_xg28` after discovering its entire Bluetooth stack was a
transitive dependency of its Range Test application components -- removing
Range Test would have silently pruned Bluetooth along with it. All three
stay on disk; only `orangelink_xg28` is live going forward.

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

- **The radio PHY is configured, now in `orangelink_xg28`.** Originally built
  in `rail_soc_railtest` (`RAIL - SoC RAILtest` example, bare RAIL, kept as
  the `RAIL_*` API reference), re-applied field-for-field to
  `rail_bt_dmp_soc_range_test`'s own Radio Configurator, then carried forward
  again (by copying the verified `config/rail/radio_settings.radioconf` file
  directly, not redone by hand) into `orangelink_xg28` when that project
  superseded it. Every field checked against the working
  RFM69 driver's own `rf69_cfg_916[]` register table (from `orangelink-ncs`,
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

  Confirmed by reading `rail_bt_dmp_soc_range_test`'s real saved
  `config/rail/radio_settings.radioconf` after saving in Studio (not just
  trusting the GUI fields) -- every value above is a real
  `<profile_inputs>` entry there (`payload_crc_en: false`,
  `base_frequency_hz: 916000000`, `syncword_0: 4278255360` = `0xFF00FF00`,
  etc.), and the channel entry is named `MDT_OOK`, generating the symbol
  `RAIL0_MDT_OOK_PROFILE_BASE` in `autogen/rail_config.h`. One real,
  worth-noting detail from reading the generated init glue while confirming
  this: `autogen/rail_config.c` emits the legacy `RAIL_ChannelConfig_t` type
  regardless of which `rail_util_init` component variant a project uses
  (Radio Configurator output format appears fixed to `rail_api_2.x`,
  independent of the `RAIL_*`/`sl_rail_*` question elsewhere), and
  `autogen/sl_rail_util_init.c` bridges it with an explicit
  `(const sl_rail_channel_config_t *)` cast before calling
  `sl_rail_config_channels()` -- Silicon Labs' own generated code doing the
  two-API-family bridging internally. Does not affect this driver, which
  never touches `channelConfigs[]` directly (goes through
  `sl_rail_util_init()`/`sl_rail_util_get_handle()`), but confirms the two
  families are meant to coexist this way rather than the project generation
  being inconsistent.

- **The encoding layer is ported verbatim.** `src/encoding/4b6b.{c,h}` and
  `manchester.{c,h}` are byte-identical copies from `orangelink-ncs`
  (`feather-nrf52832` branch) -- they had zero Zephyr dependency there and
  need no changes here.

- **A radio driver exists**, `src/drivers/rail/sl_subg_radio.{c,h}`, written
  against real `sl_rail_*()` signatures and real Micrium OS primitives, both
  checked against the actual generated `rail_bt_dmp_soc_range_test` project
  on disk (`SimplicityStudio/v6_workspace/rail_bt_dmp_soc_range_test/`) --
  the real Bluetooth+RAIL DMP project this driver is meant to integrate with,
  not the bare RAIL-only `rail_soc_railtest` used for earlier iterations.
  **Verified to compile clean** against that project's real
  toolchain/include/define set (`tools/check_compile.sh`).

  Getting here took two real, distinct corrections on top of two more from
  the previous iteration -- all four are worth keeping on record rather than
  quietly overwriting, because each was caused by a different, genuine
  ambiguity in the SDK rather than carelessness:

  1. **API family, corrected a second time: `sl_rail_*`, not `RAIL_*`.**
     Two pieces of real generated evidence exist in this repo's history and
     point different directions -- not a contradiction, but a discriminator
     by SDK component. `rail_soc_railtest` pulls in component id
     `rail_util_init` and its generated `autogen/sl_rail_util_init.c` calls
     `RAIL_Init`/`RAIL_ConfigChannels` throughout (confirmed by reading that
     file -- this drove the *first* correction, from an initial wrong guess
     of `sl_rail_*`, and was genuinely correct for that project). But
     `rail_bt_dmp_soc_range_test` -- the project this driver actually needs
     to integrate with -- pulls in a *differently-named* component,
     `sl_rail_util_init`, whose generated `autogen/sl_rail_util_callbacks.c`
     defines a real (not example/commented-out) `sl_rail_util_on_event
     (sl_rail_handle_t, sl_rail_events_t)` as the weak stub this driver's
     callback overrides, and whose `autogen/sl_rail_util_init.h` declares
     `sl_rail_util_get_handle()` returning `sl_rail_handle_t`. Every
     `sl_rail_*()` signature the driver calls (`set_tx_fifo`/`set_rx_fifo`/
     `write_tx_fifo`/`read_rx_fifo`/`start_tx`/`start_rx`/`idle`/`get_rssi`/
     `set_tx_power_dbm`/`config_events`) was checked against the real
     `sl_rail.h` in that project's own copied SDK, not extrapolated from the
     `RAIL_*` names -- some differ by more than casing (e.g.
     `sl_rail_get_rssi()` takes a microsecond wait-timeout, not the
     `RAIL_GetRssi()` bool it replaces; the FIFO setters take a
     `sl_rail_fifo_buffer_align_t*`, a `uint32_t` alias requiring
     word-array-typed buffers, not a bare `uint8_t*`).

  2. **RTOS, corrected a second time: Micrium OS, not FreeRTOS.** The
     previous iteration concluded FreeRTOS from the `bt_rail_dmp_soc_empty`
     example, which ships independent FreeRTOS and Micrium OS project
     variants. That doesn't generalize: `rail_bt_dmp_soc_range_test`'s own
     manifest selects the RTOS by silicon series --
     `micriumos_kernel, condition: [device_series_2]` /
     `freertos, condition: [device_series_3]` -- and the EFR32xG28 is Series
     2, so this project (the one actually generated for BRD2705A) only ever
     gets Micrium OS. The former busy-spin placeholders (RX-data-or-timeout,
     TX-done) are now real Micrium OS event flags (`OSFlagPend`/`OSFlagPost`),
     grounded against the real `os.h` in this project's own copied SDK.
     `OSFlagPost()` is callable from both ISR and task context in Micrium OS
     III (unlike FreeRTOS's separate `*FromISR()` family), confirmed from
     `os.h`'s own `SL_CODE_CLASSIFY` annotations -- so, unlike the FreeRTOS
     version, no ISR/task-context API split was needed. `timeout == 0` means
     "wait forever" for `OSFlagPend()` (confirmed from the real Doxygen
     comment in `os_flag.c`), which conveniently matches
     `sl_subg_get_pkt()`'s own contract with no sentinel translation needed.
     Millisecond-to-tick conversion calls the real `OSTimeTickRateHzGet()` at
     runtime rather than assuming a compile-time tick rate, since
     `OS_CFG_TICK_RATE_HZ` was not found defined anywhere in this project's
     own config/autogen/cmake output (unlike FreeRTOS's
     `configTICK_RATE_HZ`, a plain header define) -- guessing 1000 Hz here
     would have been exactly the kind of unverified assumption this project
     has already had to correct twice.

  Both corrections were only possible because a real
  `rail_bt_dmp_soc_range_test` project now exists on disk for BRD2705A --
  see "What is not started" in the previous revision of this README for why
  that was the blocking next step, and the Provenance section below for how
  it was chosen (it was NOT the first DMP example tried; `Connect Bluetooth
  DMP - SoC Empty` was created first and is a different Silicon Labs stack,
  Connect, layered over RAIL -- wrong for a project that needs unmediated
  RAIL access for its own hand-built packet framing).

  Carried over, unchanged in substance from earlier verification:

  - The event callback wiring pattern (weak-symbol override of
    `sl_rail_util_on_event()`, confirmed again in this project's own
    generated `autogen/sl_rail_util_callbacks.c`).
  - A real latent bug fixed during the Micrium OS rewrite: a full 107-byte
    receive with no `0x00` terminator never set the "have data" flag, since
    only the terminator path did.

  Real, still-open gaps, marked rather than papered over:

  - Frequency control (`sl_subg_set_freq`/`sl_subg_get_freq`) maps a
    requested Hz onto the nearest channel of the static channel config
    (`SL_SUBG_BASE_FREQ_HZ` + n * `SL_SUBG_CHANNEL_SPACING_HZ`, channels
    0-20) rather than reconfiguring the PHY -- RAIL has no arbitrary-Hz tune,
    only channel selection.
  - TX power control calls `sl_rail_set_tx_power_dbm()` (deci-dBm units, not
    the RFM69 driver's raw PA register value). Not hardware-verified, and
    depends on TX power having already been configured during
    `sl_rail_util_init()` -- not independently confirmed for this exact
    project's autogen output.

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
    macros are gone (this is not a Zephyr build). **Dispatch runs on a real
    Micrium OS task and `OS_Q`** -- the direct structural equivalent of the
    source's `k_thread`/`k_msgq` (depth 4, matching), corrected from an
    earlier FreeRTOS version once `rail_bt_dmp_soc_range_test` confirmed
    Micrium OS is what this project actually uses (see the driver's RTOS
    note). One real structural difference from the FreeRTOS version:
    Micrium OS's `OSQPost()` posts a *pointer*, not a value copy the way
    FreeRTOS's `xQueueSend()` did -- so `aps_put_cmd()` copies into one of
    `APS_QUEUE_DEPTH` static pool slots (sized to exactly match the queue
    depth, so a slot is never reused while still queued) and posts a pointer
    to that slot. `aps_put_cmd()` still enqueues non-blocking and returns
    immediately -- `OSQPost()` never blocks the poster in Micrium OS, so this
    needed no extra work to preserve the source's `K_NO_WAIT` property (the
    BLE GATT write callback, once that layer exists, is not blocked for the
    duration of a long listen). The dispatch task's priority
    (`APS_TASK_PRIORITY`) is still a placeholder, not yet checked against
    this project's real Bluetooth task priorities the way the source's own
    comment says it must sit below (in Micrium OS's convention, numerically
    *above* -- confirmed from `os.h`: `OS_PRIO_INIT` is defined as
    `OS_CFG_PRIO_MAX`, the "unassigned" sentinel, implying the numeric max is
    the least-urgent end of the range).

  Verified to compile clean, zero warnings, alongside the driver
  (`tools/check_compile.sh` now covers `src/aps/*.c` too) against
  `rail_bt_dmp_soc_range_test`'s real generated build flags -- no more
  weaker-verification-tier caveat: that project genuinely exists on disk now
  and includes Micrium OS in its own `cmake_gcc/*.cmake` output, so the
  earlier script's separate FreeRTOS-header-hunting section is gone entirely.

- **A custom BLE GATT service exists**, hand-authored directly into
  `orangelink_xg28`'s `config/btconf/gatt_configuration.btconf` and confirmed
  via the real regenerated `autogen/gatt_db.c`/`.h` after a Studio rebuild --
  not yet wired to any application code. Reproduces the legacy RileyLink
  "Insulin Pump Service" (IPS) GATT layout **exactly** (same service and
  characteristic UUIDs, properties, lengths) so AndroidAPS/Loop see the same
  surface they already know how to talk to, per
  `orangelink-ncs:feather-nrf52832`'s `docs/gatt-service-spec.md`
  (itself reconstructed from the shipping nRF5 SDK firmware):

  | Characteristic | UUID | Properties | Length |
  |---|---|---|---|
  | Data | `C842E849-5028-42E2-867C-016ADADA9155` | Read, Write | ≤150, variable |
  | Response Count | `6E6C7910-B89E-43A5-A0FE-50C5E2B81F4A` | Read, Notify | 1 |
  | Timer Tick | `6E6C7910-B89E-43A5-78AF-50C5E2B86F7E` | Read, Notify | 1 |
  | Custom Name | `D93B2AF0-1E28-11E4-8C21-0800200C9A66` | Read, Write | ≤30, variable |
  | Version | `30D99DC9-7C91-4295-A051-0A104D238CF2` | Read | 13, fixed `"ble_rfspy 2.0"` |
  | LED Mode | `C6D84241-F1A7-4F9C-A25F-FCE16732F14E` | Read, Write | 1 |

  Service UUID `0235733B-99C5-4197-B856-69219C2A3845`. The `.btconf` XML was
  authored by hand rather than through the GATT Configurator GUI (entering
  six 128-bit UUIDs by hand in the GUI is exactly the kind of tedious,
  error-prone work worth automating) -- validated well-formed before ever
  touching Studio, then confirmed correct a second time by reading the real
  compiled `gattdb_ips_*` symbols and lengths in the regenerated
  `autogen/gatt_db.h` after Studio rebuilt from it. Device name also set to
  `ClaudeLinkSI`, distinct from the existing nRF52832/nRF52840 boards'
  `ClaudeLink` name (a real prior incident had two identically-named boards
  plus an unrelated third device all advertising as `ClaudeLink` on the same
  bench, wasting a whole session targeting the wrong one).

  **Not yet done:** no application code reads from or writes to any of
  these characteristics. `aps_put_cmd()` needs to be called from a GATT
  write event on Data, and `aps_transport_send()` needs a real
  implementation -- store the response as the Data value, then increment and
  notify Response Count, in that order (the ordering is load-bearing: the
  legacy client reads Data only after seeing the Response Count
  notification).

## What is not started

- **The BLE↔APS wiring itself** -- `app_bluetooth.c` (a new file, following
  the pattern in `orangelink_xg28`'s own generated `app_bluetooth.c`
  skeleton) needs to handle `sl_bt_evt_gatt_server_attribute_value_id` for
  the Data characteristic (calling `aps_put_cmd()`) and implement
  `aps_transport_send()` as the Data-write/Response-Count-notify handshake
  described above. This is the actual remaining gap between "GATT service
  exists" and "a BLE client can talk to this firmware."
- **`APS_TASK_PRIORITY` needs tuning** against this project's real Bluetooth
  task priorities -- not yet read from the generated project.
- Nothing has run on real hardware. The board has not arrived yet.

## Provenance

Every RAIL/RTOS fact in this README and in the driver/protocol-layer
comments was checked against a real source on disk, not recalled from
memory:

- header/source files fetched into `orangelink-ncs-ws/modules/hal/silabs`
  this session (pinned to Zephyr's own `hal_silabs` revision,
  `f5201210afa1319ed8dd8dbe21682bcb63b25771`), or
- Simplicity Studio's own generated projects, live on this machine, all
  built against SDK Suite 2026.6.1:
  `~/SimplicityStudio/v6_workspace/rail_soc_railtest/` (bare RAIL, no
  Bluetooth/RTOS -- the original driver-skeleton/encoding-layer reference),
  `~/SimplicityStudio/v6_workspace/rail_bt_dmp_soc_range_test/` (real
  Bluetooth+RAIL DMP, Micrium OS -- caught the actual `sl_rail_*`/Micrium OS
  requirements, later superseded), and
  `~/SimplicityStudio/v6_workspace/orangelink_xg28/` (imported from the
  clean `bt_rail_dmp_soc_empty` Micrium OS example, no Range Test
  entanglement -- the current primary reference).

The second kind of source is what caught every correction on record here:
the `RAIL_*`/`sl_rail_*` mistake (header comments alone said the wrong thing,
"use sl_rail_*, RAIL_* is deprecated," in a way that sounded authoritative
but didn't match what the generator actually produced for either project),
and the FreeRTOS/Micrium OS mistake (a plausible generalization from one DMP
example that didn't hold for the one actually generated for this board).
Prefer checking the live generated project over documentation, header
comments, or even a *different* real generated project, when they could
plausibly disagree -- "real" and "generated" are each necessary but not
sufficient on their own; it has to be the real generated output of the
specific project this code integrates with. Where no source resolved
something, it is marked TODO rather than asserted.
