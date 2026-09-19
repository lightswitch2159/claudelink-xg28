# Orangelink -- EFR32xG28 (native Simplicity SDK)

Status: **compiles clean, not run yet.** No hardware exists for this board on
the bench, so nothing has executed -- but every file in `src/` now passes a
real `-fsyntax-only` compile against the exact toolchain, include paths, and
preprocessor defines Simplicity Studio's own generated project uses for this
board (`tools/check_compile.sh`, reproducible, extracts its flags directly
from the live generated project rather than hand-maintaining a copy). Zero
errors, zero warnings with `-Wall -Wextra`. As of this check, that project is
`rail_bt_dmp_soc_range_test` -- a real, generated **Bluetooth + RAIL DMP**
project for BRD2705A, not the bare RAIL-only `rail_soc_railtest` project used
earlier -- so this now verifies the actual `sl_rail_*()` API family and
Micrium OS integration this port needs, not just RAIL in isolation. Every
header include resolves, every call site matches its actual declared
signature, every constant used is real -- not just a plausible-looking
skeleton.

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
  - The radio PHY settings table above (OOK, 16.384 kbps, etc.) was
    configured in `rail_soc_railtest`'s Radio Configurator, not yet
    re-applied to `rail_bt_dmp_soc_range_test` -- that's the next concrete
    step, see "What is not started".

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

## What is not started

- BLE side entirely -- no work yet on Silicon Labs' native Bluetooth stack
  or the Dynamic Multiprotocol coexistence structure. `aps_transport_send()`
  has no real implementation to hand responses to yet.
- **Re-apply the RAIL PHY settings** (the OOK/16.384kbps/etc. table above,
  currently only configured in `rail_soc_railtest`'s Radio Configurator) to
  `rail_bt_dmp_soc_range_test`'s own Radio Configurator -- needed before this
  project's radio actually matches the pump's PHY. Needs Simplicity Studio's
  GUI, same as the original configuration was done.
- **`APS_TASK_PRIORITY` needs tuning** against this project's real Bluetooth
  task priorities (see above) -- not yet read from the generated project.
- Nothing has run on real hardware. The board has not arrived yet.

## Provenance

Every RAIL/RTOS fact in this README and in the driver/protocol-layer
comments was checked against a real source on disk, not recalled from
memory:

- header/source files fetched into `orangelink-ncs-ws/modules/hal/silabs`
  this session (pinned to Zephyr's own `hal_silabs` revision,
  `f5201210afa1319ed8dd8dbe21682bcb63b25771`), or
- Simplicity Studio's own generated projects, live on this machine:
  `~/SimplicityStudio/v6_workspace/rail_soc_railtest/` (bare RAIL, no
  Bluetooth/RTOS -- the original driver-skeleton/encoding-layer reference)
  and `~/SimplicityStudio/v6_workspace/rail_bt_dmp_soc_range_test/` (real
  Bluetooth+RAIL DMP, Micrium OS -- generated this session, now the primary
  reference for the driver and protocol layer), both built against SDK Suite
  2026.6.1.

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
