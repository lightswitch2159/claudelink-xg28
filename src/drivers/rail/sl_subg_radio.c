/*
 * Sub-GHz radio driver -- EFR32xG28 native RAIL, 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * See sl_subg_radio.h for the full history: this is the SECOND API-family
 * correction (RAIL_* -> sl_rail_*) and the SECOND RTOS correction
 * (FreeRTOS -> Micrium OS), both made this session after generating a real
 * rail_bt_dmp_soc_range_test project for BRD2705A in Simplicity Studio and
 * reading its actual autogen/ output and .slcp manifest -- not guessed, and
 * not simply "fixed back" to the original assumption; each was a distinct,
 * traceable finding (see the header banner).
 *
 * Every sl_rail_*() call below was checked against the real header in this
 * exact project's own copied SDK:
 * SimplicityStudio/v6_workspace/rail_bt_dmp_soc_range_test/simplicity_sdk_2026.6.1/rail_library/common/sl_rail.h
 * (and sl_rail_types.h for the types). Every OSFlag/OSQ/OSTask call was
 * checked against
 * .../simplicity_sdk_2026.6.1/micriumos/platform/micrium_os/kernel/include/os.h
 * in that same project.
 *
 * tools/check_compile.sh has NOT yet been re-pointed at this new project as
 * of this rewrite -- see that script's own comment for the current state.
 * Treat this as a compile-clean-pending starting point, not yet re-verified
 * the way the RAIL_* / FreeRTOS version was against rail_soc_railtest.
 */

#include <stdio.h>
#include <string.h>

#include "debug/dbg_log.h"
#include "sl_subg_radio.h"
#include "sl_rail.h"
#include "sl_rail_types.h"
#include "sl_rail_util_init.h"   /* sl_rail_util_init(), sl_rail_util_get_handle() */
#include "rail_config.h"         /* generated channelConfigs[] */

#include "os.h"
#include "rtos_err.h"

static sl_rail_handle_t s_rail_handle;

/*
 * WAIT PRIMITIVE, Micrium OS event flags -- replaces the FreeRTOS event group
 * from the previous rewrite. OSFlagPend()/OSFlagPost() are used from BOTH ISR
 * and task context in Micrium OS III (unlike FreeRTOS's separate *_FromISR()
 * API family) -- confirmed from os.h's own SL_CODE_CLASSIFY annotations on
 * OSFlagPend/OSFlagPost marking them SL_CODE_CLASS_TIME_CRITICAL, callable
 * from either context, so sl_rail_util_on_event() (interrupt context, same
 * as it was under the RAIL_* driver -- inherited from the bt_rail_dmp_soc_empty
 * example's readme.md, not re-confirmed against this project's own docs,
 * which don't cover it) and sl_subg_abort() (task context) both call
 * OSFlagPost() directly, no FromISR variant needed.
 *
 * timeout == 0 means "wait forever" for OSFlagPend() -- confirmed from the
 * real Doxygen comment in
 * .../micriumos/platform/micrium_os/kernel/source/os_flag.c ("If you specify
 * 0, the task will wait forever"). This conveniently matches
 * sl_subg_get_pkt()'s own "timeout_ms == 0 means forever" contract exactly,
 * so no special-case translation is needed the way FreeRTOS's portMAX_DELAY
 * sentinel required.
 *
 * ms-to-ticks conversion uses OSTimeTickRateHzGet() at runtime rather than a
 * compile-time constant: this project's OS_CFG_TICK_RATE_HZ was not found
 * defined anywhere in its own config/autogen/cmake output (unlike FreeRTOS's
 * configTICK_RATE_HZ, which was a plain header define) -- it appears to be
 * supplied purely as a runtime value (OSCfg_TickRate_Hz), so guessing a
 * compile-time 1000 Hz here would be exactly the kind of unverified
 * assumption this project has twice already had to correct. Real, checked
 * signature: OS_RATE_HZ OSTimeTickRateHzGet(RTOS_ERR *p_err).
 */
#define SL_SUBG_EVT_RX_DATA ((OS_FLAGS)(1u << 0))
#define SL_SUBG_EVT_TX_DONE ((OS_FLAGS)(1u << 1))
#define SL_SUBG_EVT_ABORT   ((OS_FLAGS)(1u << 2))

/* Legacy TX_TIMEOUT (subg.c: SUBG_TX_DONE_WAIT_MS 150) -- a TX that never
 * completes must not hang the caller forever the way sl_subg_get_pkt()'s
 * 0-means-forever timeout legitimately can for RX.
 */
#define SL_SUBG_TX_DONE_TIMEOUT_MS 150U

/* The configured PHY is 16.384 kbps with a 128-bit preamble and 32-bit sync
 * word. Include those 160 bits plus a 2 ms margin in the scheduler estimate. */
#define SL_SUBG_BITRATE_BPS           16384U
#define SL_SUBG_PREAMBLE_SYNC_BITS    160U
#define SL_SUBG_TX_MARGIN_US          2000U
#define SL_SUBG_TX_SLIP_TIME_US       100000U
/* Same formula as the TX scheduler estimate below, sized for the longest
 * possible single reception (SL_SUBG_MAX_PKT_LEN) rather than a known TX
 * length, since RX doesn't know the incoming frame's length in advance.
 * See sl_subg_get_pkt()'s own comment for why this exists.
 */
#define SL_SUBG_RX_MARGIN_US          5000U
/* Re-arm interval for sl_subg_get_pkt()'s listen loop -- see that function's
 * own comment for why a single long-lived sl_rail_start_rx() call across
 * the whole caller-supplied timeout doesn't work in practice. Longer than
 * one full max-length frame at this bitrate (~62 ms) so a real, complete
 * reception is never cut off mid-frame by a re-arm; short enough to give
 * many fresh listen attempts within a typical AndroidAPS wake/scan window.
 *
 * TRIED 5000 FOR A DIRECT A/B TEST AGAINST A REFERENCE BRIDGE, REVERTED:
 * the reference bridge caught a clean reply on 5/5 commands with no
 * retries (400-500 ms round trip, HackRF-confirmed). Raising this value to
 * 5000 to cut re-arm dead-zone count was the resulting hypothesis, tested
 * with a 130 s HackRF-ground-truth capture correlated against the AAPS
 * production log covering the same window (bench_646910_longrearm_test_
 * 20260924.cs8): AndroidAPS's automatic isDeviceReachable wakeUp polling
 * fired 5 times in that window and failed all 5 ("Failed to find pump
 * (timeout)"), despite the bridge's own wakeup TX clearly going out on air
 * every time. Only one genuine pump reply appeared in the whole capture,
 * before any logged wakeUp attempt even started. That is not an
 * improvement over the 300 ms baseline, which had already produced a full
 * PumpConnectorReady session and a 25/25 clean capture in earlier testing
 * this same session -- so the dead-zone-duration hypothesis above is not
 * supported by this evidence, and 300 is restored as the working value.
 * Left at 300 pending the next real experiment (a genuinely continuous,
 * non-retuning single RX arm for one bounded AndroidAPS listen window,
 * instead of just a longer period between the same periodic re-arms).
 */
#define SL_SUBG_RX_REARM_MS           300U
/* Grace period granted to an in-progress reception (bytes already
 * arriving, no terminator yet) when the re-arm timer above expires before
 * it finishes -- see sl_subg_get_pkt()'s own comment for why. Comfortably
 * longer than one full max-length frame (~62 ms) past whatever portion of
 * the 300 ms slice was already spent waiting.
 */
#define SL_SUBG_RX_GRACE_MS           150U

/*
 * REAL BUG, FOUND AGAINST A PRODUCTION BRIDGE'S wakeUp() PHASE: HackRF ground
 * truth during a live AndroidAPS wakeUp() call (25000 ms listen) showed the
 * bench pump transmitting short 7-byte wake-ack frames on a fairly steady
 * ~200-400 ms cadence throughout the window -- comfortably frequent relative
 * to the fixed 300 ms re-arm period above -- yet this driver reported
 * SL_SUBG_RX_TIMEOUT the entire time, never capturing a single one, despite
 * the exact same re-arm loop proving reliable during AndroidAPS's own
 * 1250 ms scanForDevice() tries (32/33 clean re-arms in the same session).
 * The difference between the two is re-arm COUNT, not code path: a 1250 ms
 * listen only gets ~4 re-arms, a 25000 ms listen gets ~83 -- and a FIXED
 * re-arm period that happens to sit near-harmonic with the pump's own
 * near-fixed ack interval can put the brief (~5 ms on-air) ack in this
 * loop's per-re-arm dead zone (FIFO reset, retune, arm) on every single
 * re-arm instead of eventually drifting into coverage, the way a few
 * uncorrelated re-arms would. Confirmed independently against a real
 * production bridge's own AndroidAPS log the same session: two distinct
 * multi-retry wakeUp failures, both showing a DIFFERENT nearby pump's
 * wake-ack winning every single retry in a row -- the same signature of a
 * fixed-period listener missing a periodic transmitter again and again
 * rather than the expected random spread of hits and misses. Jittering the
 * re-arm period breaks any such alignment: no two consecutive re-arms (or
 * any short run of them) sit at the same phase relative to an external
 * periodic transmitter, so a periodic ack that is being missed on every
 * attempt at a fixed period gets a genuinely different chance each time
 * instead of the same missed chance repeated. The jitter sequence itself
 * doesn't need real entropy -- multiplying by an odd constant and taking it
 * mod a range not sharing a small common factor with typical ack intervals
 * (101 is prime) is enough to avoid resonance; a hardware RNG would just add
 * a blocking call for no real benefit here.
 *
 * REAL BUG IN THIS FIX'S OWN FIRST VERSION, CAUGHT LIVE AGAINST THE BENCH
 * PUMP: seeding the jitter from rearm_count (reset to 0 at the top of every
 * sl_subg_get_pkt() call) only varies the re-arm phase WITHIN one call, not
 * BETWEEN separate wakeUp attempts -- every fresh call starts the same
 * sequence over again. Live capture showed the pump's actual reply landing
 * a near-constant ~1-2 s after each wake command across four consecutive
 * AndroidAPS wakeUp() retries, i.e. the thing that needed to vary between
 * attempts was exactly the thing the old seed held constant. A free-running
 * counter that never resets -- incremented once per re-arm across the
 * device's entire uptime, not per call -- gives each new wakeUp attempt a
 * different starting phase against that same ~1-2 s post-wake window
 * instead of replaying the same one.
 */
#define SL_SUBG_RX_REARM_JITTER_MIN_MS 250U
#define SL_SUBG_RX_REARM_JITTER_RANGE_MS 101U /* prime -- see comment above */

static uint32_t s_rx_rearm_jitter_seed;

static uint32_t sl_subg_rx_rearm_ms_jittered(void)
{
	uint32_t jitter = (s_rx_rearm_jitter_seed * 73U) % SL_SUBG_RX_REARM_JITTER_RANGE_MS;

	s_rx_rearm_jitter_seed++;
	return SL_SUBG_RX_REARM_JITTER_MIN_MS + jitter;
}

static OS_FLAG_GRP s_event_flags;

static OS_TICK ms_to_ticks(uint32_t ms)
{
	RTOS_ERR err;
	OS_RATE_HZ rate_hz;

	if (ms == 0) {
		return 0;   /* 0 stays 0: "wait forever" on both sides of this call. */
	}

	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
	rate_hz = OSTimeTickRateHzGet(&err);
	if (RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE || rate_hz == 0) {
		return (OS_TICK)ms;   /* best-effort fallback, not a verified rate */
	}

	return (OS_TICK)(((uint64_t)ms * rate_hz) / 1000U);
}

/* TX/RX FIFO buffers. sl_rail_set_tx_fifo()/sl_rail_set_rx_fifo() take a
 * sl_rail_fifo_buffer_align_t* (sl_rail_types.h: typedef uint32_t
 * sl_rail_fifo_buffer_align_t), not a bare uint8_t* the way the RAIL_*
 * family's RAIL_SetTxFifo()/RAIL_SetRxFifo() did -- confirmed from the real
 * signatures, not assumed. Byte count stays SL_SUBG_FIFO_BYTES; the array
 * element count is divided by 4 accordingly. 128 is a multiple of 4, so no
 * padding/remainder concern here.
 */
#define SL_SUBG_FIFO_BYTES 128U
static sl_rail_fifo_buffer_align_t s_tx_fifo[SL_SUBG_FIFO_BYTES / sizeof(sl_rail_fifo_buffer_align_t)];
static sl_rail_fifo_buffer_align_t s_rx_fifo[SL_SUBG_FIFO_BYTES / sizeof(sl_rail_fifo_buffer_align_t)];

static uint8_t s_rx_buf[SL_SUBG_MAX_PKT_LEN];
static uint8_t s_rx_count;
static volatile bool s_rx_have_data;
static volatile bool s_tx_done;
static volatile bool s_tx_underflow;
static volatile bool s_abort_flag;
/* Raw OR of any RX_PACKET_ABORTED/RX_FRAME_ERROR/RX_FIFO_OVERFLOW events
 * seen during the current sl_subg_get_pkt() call -- see that function's own
 * comment on why this is recorded here (ISR context) but only printed there
 * (task context). Cleared at the start of each call.
 */
static volatile sl_rail_events_t s_rx_error_events;
/* RSSI captured in the ISR at the exact moment an RX error/timing event
 * fires -- see sl_rail_util_on_event()'s own comment on why this exists
 * separately from the end-of-slice s_last_rssi_dbm read. INT16_MIN is the
 * "never set this iteration" sentinel (distinct from RAIL's own
 * SL_RAIL_RSSI_INVALID, which is a valid observed reading of -128 dBm and
 * must not be confused with "no error event happened").
 */
static volatile int16_t s_rx_error_rssi_dbm = INT16_MIN;
static int16_t s_last_rssi_dbm = INT16_MIN;
static uint16_t s_rx_pkt_count;
static uint16_t s_tx_pkt_count;
static uint16_t s_channel = SL_SUBG_CHANNEL;
static uint32_t s_frequency_hz = SL_SUBG_FREQ_HZ;

/* RAIL's channel configuration is cached by pointer, so keep this runtime
 * single-channel map in static storage. Clone the generated PHY and channel
 * attributes, then update only the frequency for each host tuning request.
 */
static RAIL_ChannelConfig_t s_frequency_channel_config;
static RAIL_ChannelConfigEntry_t s_frequency_channel_entry;
static bool s_frequency_channel_config_ready;

/*
 * RAIL event callback -- the equivalent of the RFM69 port's DIO1 ISR
 * (dio1_handler() in rf69.c), except RAIL delivers this as a proper event
 * bitmask rather than a single GPIO edge, so there is no separate "which
 * interrupt fired" step.
 *
 * SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL is the direct analogue of DIO1 mapped to
 * FifoNotEmpty: it fires as bytes become available, before the packet (by our
 * own fixed-length declaration, up to 107 bytes) is complete. Draining here
 * and watching for the 0x00 terminator reproduces subg_get_pkt()'s existing
 * "terminate on a sentinel byte, do not wait for the radio's own notion of
 * packet-complete" behaviour.
 *
 * WIRING: this function is deliberately named sl_rail_util_on_event and given
 * external (non-static) linkage to override the __WEAK stub of the same name
 * in the generated autogen/sl_rail_util_callbacks.c of
 * rail_bt_dmp_soc_range_test -- confirmed by reading that file directly this
 * session (it declares exactly this signature,
 * sl_rail_handle_t/sl_rail_events_t, gated on
 * SL_CATALOG_SL_RAIL_UTIL_CALLBACKS_PRESENT, which this project's component
 * selection defines). Its own internal sli_rail_util_on_event(), which IS
 * what sl_rail_util_init() wires into the real event dispatch, does nothing
 * but call sl_rail_util_on_event(); the weak default is an empty stub, and
 * that generated file's own header warns edits there are discarded on
 * regeneration -- the intended pattern really is a weak-symbol override
 * living outside autogen/. sl_rail_config_events() in sl_subg_radio_init()
 * below is still required, separately -- it controls which event bits are
 * unmasked at the radio, not which function receives them.
 */
void sl_rail_util_on_event(sl_rail_handle_t handle, sl_rail_events_t events)
{
	bool rx_done = false;
	RTOS_ERR err;

	/*
	 * REAL BUG, CAUGHT ON LIVE HARDWARE, NOT A GUESS: this receive already
	 * completed (terminator seen, or buffer full) for the current
	 * sl_subg_get_pkt() call, but s_rx_have_data only resets at the START
	 * of the NEXT call -- there is no guard stopping a second
	 * RX_FIFO_ALMOST_FULL firing (e.g. a second, unrelated over-the-air
	 * burst arriving before the task wakes up and calls sl_rail_idle())
	 * from draining MORE bytes into s_rx_buf at the current s_rx_count,
	 * silently appending an unrelated frame onto the end of a real one.
	 * Confirmed exactly this: a live probe against the bench pump
	 * (2026-09-23) received a correctly-decoded 646910 model reply
	 * immediately followed by a second pump's frame header concatenated
	 * onto it in the same buffer, failing the CRC check downstream. The
	 * pump's real reply was never actually missing -- it was being
	 * silently corrupted by trailing garbage after arrival.
	 */
	if ((events & SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL) && !s_rx_have_data) {
		while (s_rx_count < SL_SUBG_MAX_PKT_LEN) {
			uint8_t byte;
			uint16_t got = sl_rail_read_rx_fifo(handle, &byte, 1);

			if (got == 0) {
				break;
			}
			if (byte == 0x00) {
				/* Terminator -- same convention as subg_get_pkt(). */
				s_rx_have_data = true;
				rx_done = true;
				break;
			}
			s_rx_buf[s_rx_count++] = byte;
		}

		/* A full buffer with no terminator seen is still a completed receive
		 * -- subg.c's equivalent loop breaks on EITHER condition
		 * (`count >= SUBG_MAX_PKT_LEN`), not terminator alone.
		 */
		if (!rx_done && s_rx_count >= SL_SUBG_MAX_PKT_LEN) {
			s_rx_have_data = true;
			rx_done = true;
		}
	}

	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);

	if (rx_done) {
		(void)OSFlagPost(&s_event_flags, SL_SUBG_EVT_RX_DATA, OS_OPT_POST_FLAG_SET, &err);
	}

	if (events & SL_RAIL_EVENT_TX_PACKET_SENT) {
		s_tx_done = true;
		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		(void)OSFlagPost(&s_event_flags, SL_SUBG_EVT_TX_DONE, OS_OPT_POST_FLAG_SET, &err);
	}
	if (events & SL_RAIL_EVENT_TX_UNDERFLOW) {
		s_tx_underflow = true;
	}

	/*
	 * Investigating a real bench-hardware finding: HackRF independently
	 * decoded 18 CRC-valid pump replies during a live test window this
	 * firmware's own rx_packets counter never incremented for -- so
	 * something is destroying or never surfacing an in-flight receive
	 * before sl_subg_get_pkt() sees it. This is a Dynamic Multiprotocol
	 * build, and the RAIL SDK's own sl_rail_config_rx_data() doc carries
	 * a direct note: "When using multiprotocol, if a protocol's receive
	 * FIFO or receive Packet Queue is shared with another protocol, they
	 * will be reset during a protocol switch" -- exactly what a BLE
	 * connection event landing mid-packet (our RX priority is
	 * deliberately low, 200, so BLE can preempt it) would do. These three
	 * events are what RAIL fires when that happens; none were enabled or
	 * recorded before. Recorded here (ISR context) rather than printed --
	 * see sl_subg_get_pkt()'s own comment for why the printf happens
	 * there instead.
	 */
	/*
	 * SL_RAIL_EVENT_RX_TIMING_LOST added while chasing why 646910's own
	 * reply never produces a clean receive even with everything else in
	 * this file fixed (guard against post-terminator appending, periodic
	 * re-arm, FIFO reset before arming, a 50 ms TX-to-RX settling delay,
	 * and a grace period for a reception already in progress) -- see
	 * DEBUGGING_NOTES_2026-09-23.md Follow-up 6. If RAIL itself notices
	 * losing symbol/bit timing partway through a reception (which a
	 * corrupted or undemodulated terminator would plausibly cause), this
	 * is the cheapest way to see that directly, before committing to a
	 * full RSSI-polling redesign.
	 */
	if (events & (SL_RAIL_EVENT_RX_PACKET_ABORTED | SL_RAIL_EVENT_RX_FRAME_ERROR
		      | SL_RAIL_EVENT_RX_FIFO_OVERFLOW | SL_RAIL_EVENT_RX_TIMING_LOST)) {
		/*
		 * RSSI-at-error-time, added while implementing the RSSI/carrier-
		 * sense work flagged as the next step in DEBUGGING_NOTES_2026-09-23.md
		 * (Follow-up 10/11). The existing s_last_rssi_dbm read in
		 * sl_subg_get_pkt() happens once, at the end of a whole
		 * SL_SUBG_RX_REARM_MS (300 ms) slice -- for 646910, whose failures
		 * never leave s_rx_count > 0, the grace-period extension never
		 * triggers, so that end-of-slice read is separated from the actual
		 * moment RX_TIMING_LOST fired by however much of the slice was
		 * still left. That gap matters here specifically: the open
		 * question from Follow-up 11 is whether 646910's reply is reaching
		 * the receiver at a real, above-noise-floor level and failing to
		 * demodulate (RAIL achieving timing lock at all is itself evidence
		 * of a real signal, not pure noise) versus never getting there in
		 * the first place -- and the noise floor drifts across a listen
		 * window, so only a reading taken at the actual event moment is
		 * usable as evidence either way. sl_rail_get_rssi(..., NO_WAIT) is
		 * a non-blocking register read with no averaging wait, which is
		 * the documented condition for it being safe to call from event-
		 * callback/ISR context (the same call this file already makes
		 * from task context elsewhere) -- and the radio is still in or
		 * just leaving RX here, before sl_subg_get_pkt()'s own
		 * sl_rail_idle() call, so this is the last point this value is
		 * even meaningful (afterward it would read SL_RAIL_RSSI_INVALID
		 * for the same reason the pre-existing end-of-call read had to be
		 * moved earlier in this same investigation). Only the first error
		 * event this iteration is kept -- once RX_TIMING_LOST fires the
		 * reception is already lost, and a later event's RSSI would no
		 * longer reflect the moment lock broke.
		 */
		if (s_rx_error_rssi_dbm == INT16_MIN) {
			/* Same quarter-dBm-to-dBm conversion as the existing
			 * end-of-slice read below; not special-casing
			 * SL_RAIL_RSSI_INVALID here the way that comment discusses --
			 * the radio is still in/leaving RX at this exact point (unlike
			 * the idle-after-read bug that motivated tracking it there),
			 * so seeing it here would itself be a meaningful, reportable
			 * anomaly rather than the expected case.
			 */
			int16_t rssi = sl_rail_get_rssi(handle, SL_RAIL_GET_RSSI_NO_WAIT);

			s_rx_error_rssi_dbm = (int16_t)(rssi / 4);
		}
		s_rx_error_events |= events & (SL_RAIL_EVENT_RX_PACKET_ABORTED
						| SL_RAIL_EVENT_RX_FRAME_ERROR
						| SL_RAIL_EVENT_RX_FIFO_OVERFLOW
						| SL_RAIL_EVENT_RX_TIMING_LOST);
	}
}

int sl_subg_radio_init(void)
{
	RTOS_ERR err;
	uint16_t rx_size = SL_SUBG_FIFO_BYTES;
	sl_rail_status_t st;
	const sl_rail_rx_data_config_t rx_data_config = {
		.rx_source = SL_RAIL_RX_DATA_SOURCE_PACKET_DATA,
		.rx_method = SL_RAIL_DATA_METHOD_FIFO_MODE,
	};

	/*
	 * Do NOT call sl_rail_util_init() here -- REAL HARDWARE BUG FOUND AND
	 * FIXED THIS SESSION, not a guess. Silicon Labs' generated
	 * autogen/sl_event_handler.c already calls it once, automatically, from
	 * sl_stack_init(), which the framework runs before app_init() (and
	 * therefore before sl_subg_radio_init(), which app.c calls from
	 * app_init()) on every boot. Calling it a second time here re-ran
	 * sl_rail_init() against an already-initialized RAIL instance, which
	 * returned SL_STATUS_INVALID_PARAMETER (0x0021) -- confirmed on real
	 * hardware via RTT-connected printf checkpoints (see git history for the
	 * diagnostic session) -- and that failure trips the generated
	 * sl_rail_util_init_inst0()'s own APP_ASSERT(), which hangs forever
	 * (silently: this project has no app_log component, so app_assert's
	 * failure path is the printless infinite-loop variant -- see
	 * app_assert.h). That hang, before anything RAIL- or BLE-related ever
	 * ran, was the very first symptom this bug produced: total silence, no
	 * BLE advertisement, nothing.
	 *
	 * Just retrieve the handle the framework already brought up -- the same
	 * pattern already used (in a comment, not compiled) in the
	 * bt_rail_dmp_soc_empty example's own app_proprietary.c: get the handle,
	 * do not re-init.
	 */
	s_rail_handle = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
	if (s_rail_handle == SL_RAIL_EFR32_HANDLE) {
		return -1;
	}

	/*
	 * Diagnostic: a bench pump confirmed at less than a foot away, on a
	 * fresh battery, still measured at the noise floor (~-100 dBm,
	 * DEBUGGING_NOTES_2026-09-23.md Follow-up 8) -- at that range, free-
	 * space path loss is negligible, so a genuinely working transmitter
	 * should read tens of dB above the floor. sl_rail_set_rssi_offset()'s
	 * own doc says RSSI carries "a per-PHY offset set by the radio
	 * calculator" in addition to anything set explicitly -- this driver
	 * has never called sl_rail_set_rssi_offset() itself, so logging
	 * whatever offset is already in effect checks whether our own
	 * reported RSSI is even meaningful in absolute terms before
	 * suspecting the pump or the antenna.
	 */
	dbg_printf("radio: RSSI offset = %d dB\r\n", sl_rail_get_rssi_offset(s_rail_handle));

	/* Static allocation, matching the source's K_SEM_DEFINE-style static
	 * allocation rather than the heap. Must exist before
	 * sl_rail_config_events() below unmasks events that could otherwise race
	 * this driver's own init.
	 */
	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
	OSFlagCreate(&s_event_flags, "subg radio", 0, &err);
	if (RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE) {
		return -1;
	}

	/* The RX callback consumes bytes as they arrive and terminates on the
	 * Minimed zero sentinel. Packet mode (RAIL's default) disables FIFO
	 * threshold events, so the existing RX_FIFO_ALMOST_FULL handler would never
	 * run even though it is enabled below. Select FIFO mode explicitly.
	 */
	st = sl_rail_config_rx_data(s_rail_handle, &rx_data_config);
	if (st != SL_RAIL_STATUS_NO_ERROR) {
		dbg_printf("radio: RX FIFO mode config failed (status 0x%04lx)\r\n",
		       (unsigned long)st);
		return -1;
	}

	st = sl_rail_set_tx_fifo(s_rail_handle, s_tx_fifo, SL_SUBG_FIFO_BYTES, 0, 0);
	if (st != SL_STATUS_OK) {
		return -1;
	}

	st = sl_rail_set_rx_fifo(s_rail_handle, s_rx_fifo, &rx_size);
	if (st != SL_STATUS_OK) {
		return -1;
	}

	/* The legacy Orangelink RFM69 config uses PA0 at output level 31,
	 * approximately +13 dBm. The xG28 PA utility's project default is only
	 * +10 dBm, so select the legacy-equivalent output explicitly after PA
	 * initialization. sl_rail_util_pa_init() runs in sl_stack_init() before
	 * app_init() reaches this function.
	 */
	if (sl_subg_set_power_level(13) != 0) {
		dbg_printf("radio: failed to set TX power to +13 dBm\r\n");
		return -1;
	}

	/* Minimed replies are short, zero-terminated variable-length frames.
	 * A half-FIFO threshold suppresses the only event we currently use to
	 * drain RX for every reply shorter than 65 bytes, so they silently time
	 * out. RAIL keeps RX_FIFO_ALMOST_FULL asserted while FIFO occupancy is
	 * above the threshold; drain from a low threshold to see short frames as
	 * they arrive and stop immediately at their zero terminator.
	 */
	{
		/* Return value previously discarded -- same class of gap as the
		 * TX FIFO/start checks below. sl_rail_set_rx_fifo_threshold()
		 * echoes back the threshold it actually configured (mirrors
		 * sl_rail_set_fixed_length()'s contract), so a silent failure
		 * to apply 1 here would look identical to "RX just isn't
		 * getting bytes" -- exactly the live bug under investigation.
		 */
		uint16_t applied = sl_rail_set_rx_fifo_threshold(s_rail_handle, 1U);

		if (applied != 1U) {
			dbg_printf("radio: RX FIFO threshold got %u, not 1\r\n", applied);
		}
	}

	/* RX_PACKET_ABORTED/RX_FRAME_ERROR/RX_FIFO_OVERFLOW newly enabled --
	 * see sl_rail_util_on_event()'s own comment on why: FIFO mode never
	 * rolls back data on these, but also never tells the app when they
	 * happen unless it asks, and this driver never asked. A Dynamic
	 * Multiprotocol protocol-switch resetting our shared RX FIFO mid-packet
	 * would surface as one of these.
	 */
	st = sl_rail_config_events(s_rail_handle,
				   SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL | SL_RAIL_EVENT_TX_PACKET_SENT
				   | SL_RAIL_EVENT_TX_UNDERFLOW | SL_RAIL_EVENT_RX_PACKET_ABORTED
				   | SL_RAIL_EVENT_RX_FRAME_ERROR | SL_RAIL_EVENT_RX_FIFO_OVERFLOW
				   | SL_RAIL_EVENT_RX_TIMING_LOST,
				   SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL | SL_RAIL_EVENT_TX_PACKET_SENT
				   | SL_RAIL_EVENT_TX_UNDERFLOW | SL_RAIL_EVENT_RX_PACKET_ABORTED
				   | SL_RAIL_EVENT_RX_FRAME_ERROR | SL_RAIL_EVENT_RX_FIFO_OVERFLOW
				   | SL_RAIL_EVENT_RX_TIMING_LOST);
	if (st != SL_STATUS_OK) {
		return -1;
	}

	/* sl_rail_util_on_event() above overrides the generated weak stub by
	 * symbol name -- nothing to call or register here.
	 */

	/*
	 * REAL BUG, caught live while verifying the RX frequency sweep: without
	 * this, s_frequency_channel_config_ready (sl_subg_set_freq()'s own
	 * lazy-init guard) stays false until AndroidAPS sends its first
	 * CMD_UPDATE_REG for the frequency registers -- but AndroidAPS's own
	 * scanForDevice() calls wakeUp() first, which sends its own
	 * send+listen exchanges *before* the per-candidate-frequency loop
	 * (with its setBaseFrequency() calls) ever runs. RTT confirmed this
	 * directly: sl_subg_retune_rx_only() returned -1 on every single
	 * re-arm of the very first exchanges after a fresh boot, exactly
	 * matching that guard. Calling sl_subg_set_freq() here with the
	 * existing built-in default (the same SL_SUBG_FREQ_HZ
	 * sl_subg_reset_radio_cfg() already uses for CMD_RESET) makes the
	 * channel config ready from boot, before any host command arrives at
	 * all -- matching the fact that this exact default is already the
	 * frequency TX/RX would silently use anyway via the compiled-in
	 * channelConfigs[0] if nothing had touched it yet.
	 */
	(void)sl_subg_set_freq(SL_SUBG_FREQ_HZ);

	return 0;
}

int sl_subg_send_pkt(const uint8_t *data, uint8_t len, uint8_t repeat_cnt,
		     uint16_t repeat_interval_ms)
{
	RTOS_ERR err;
	/*
	 * REAL GAP, found while investigating why AndroidAPS's real mmtune
	 * traffic against 560793 kept seeing very few pump replies even after
	 * every previously-found RX bug was fixed: the diagnostics below were
	 * gated to i == 0 only ("a genuine failure here is systemic, not a
	 * one-off worth repeating 200 times over RTT") -- true for a total,
	 * first-repeat failure, but it means a PARTIAL failure -- some
	 * fraction of the 200 repeats in a real wake burst silently not
	 * reaching the air, while the first one succeeds -- was completely
	 * invisible. This is a Dynamic Multiprotocol build sharing the radio
	 * with BLE; a connection event landing mid-burst is exactly the kind
	 * of thing that could cost individual repeats without failing the
	 * first one. Counting every outcome, not just the first, turns "the
	 * pump doesn't seem to reply much" from a pump-side assumption into a
	 * directly checkable bridge-side fact.
	 */
	uint16_t tx_fifo_fail_count = 0;
	uint16_t tx_start_fail_count = 0;
	uint16_t tx_done_timeout_count = 0;
	uint16_t tx_ok_count = 0;

	for (uint8_t i = 0; i <= repeat_cnt; i++) {
		uint16_t fifo_written;
		uint16_t tx_frame_len = (uint16_t)len + 1U;
		uint8_t tx_data[SL_SUBG_MAX_PKT_LEN + 1U];
		sl_rail_status_t tx_sc;
		sl_rail_scheduler_info_t scheduler_info = {
			.priority = 100,
			.slip_time = SL_SUBG_TX_SLIP_TIME_US,
			.transaction_time = (((uint32_t)tx_frame_len * 8U + SL_SUBG_PREAMBLE_SYNC_BITS)
					     * 1000000U / SL_SUBG_BITRATE_BPS)
					    + SL_SUBG_TX_MARGIN_US,
		};
		uint16_t configured_len;

		if (len == 0 || len > SL_SUBG_MAX_PKT_LEN) {
			return -1;
		}

		/* Legacy Minimed framing sends a variable number of encoded bytes,
		 * followed by a zero terminator. 107 is the maximum RX payload; it is
		 * not the length of every TX frame. RAIL uses the configured fixed
		 * length for TX, so override it to payload + terminator for each send.
		 */
		configured_len = sl_rail_set_fixed_length(s_rail_handle, tx_frame_len);
		if (configured_len != tx_frame_len) {
			dbg_printf("radio: failed to set TX frame length %u (got %u)\r\n",
			       tx_frame_len, configured_len);
			return -1;
		}
		memcpy(tx_data, data, len);
		tx_data[len] = 0;

		s_tx_done = false;
		s_tx_underflow = false;
		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		(void)OSFlagPost(&s_event_flags, SL_SUBG_EVT_TX_DONE, OS_OPT_POST_FLAG_CLR, &err);

		/* Both return values were previously discarded -- a TX that
		 * never left the radio (bad channel/FIFO state, scheduler
		 * refusal, etc.) looked externally identical to "sent fine,
		 * pump just didn't answer": sl_subg_get_pkt() below times out
		 * either way, no error, nothing to tell them apart. Checked
		 * for real (not guessed) while investigating a live pump test
		 * where AndroidAPS got no reply at all.
		 */
		fifo_written = sl_rail_write_tx_fifo(s_rail_handle, tx_data,
						    tx_frame_len, true);
		tx_sc = sl_rail_start_tx(s_rail_handle, s_channel,
					 SL_RAIL_TX_OPTIONS_DEFAULT, &scheduler_info);

		if (fifo_written != tx_frame_len) {
			tx_fifo_fail_count++;
		}
		if (tx_sc != SL_RAIL_STATUS_NO_ERROR) {
			tx_start_fail_count++;
		}
		if (i == 0 && (fifo_written != tx_frame_len || tx_sc != SL_RAIL_STATUS_NO_ERROR)) {
			/* Permanent checkpoint, not temporary -- see the comment
			 * above. First repeat only: repeat_cnt can be 200+ (a
			 * real AndroidAPS wakeup burst), and a genuine failure
			 * here is systemic, not a one-off worth repeating 200
			 * times over RTT.
			 */
			dbg_printf("radio: TX FAILED (fifo %u/%u B written, start_tx status 0x%04lx)\r\n",
			       fifo_written, tx_frame_len, (unsigned long)tx_sc);
		}

		/* Bounded, not indefinite -- unlike sl_subg_get_pkt()'s
		 * caller-supplied listen window, a TX that never completes has no
		 * legitimate "wait forever" case. Mirrors subg.c's
		 * SUBG_TX_DONE_WAIT_MS. A timeout here leaves s_tx_done false; the
		 * loop proceeds to the next repeat regardless, matching the source's
		 * "count failures, do not abandon the burst" policy at the aps.c call
		 * site rather than this function inventing its own abort behaviour.
		 */
		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		(void)OSFlagPend(&s_event_flags, SL_SUBG_EVT_TX_DONE,
				 ms_to_ticks(SL_SUBG_TX_DONE_TIMEOUT_MS),
				 OS_OPT_PEND_FLAG_SET_ANY | OS_OPT_PEND_FLAG_CONSUME | OS_OPT_PEND_BLOCKING,
				 NULL, &err);

		if (!s_tx_done) {
			tx_done_timeout_count++;
			if (i == 0) {
				dbg_printf("radio: TX_PACKET_SENT timeout%s\r\n",
				       s_tx_underflow ? " after TX_UNDERFLOW" : "");
			}
		} else {
			tx_ok_count++;
		}

		/* Instantaneous TX must yield the shared radio when it completes.
		 * On timeout, abort first so a stuck transmission cannot overlap the
		 * next repeat, then yield the radio back to the BLE scheduler. */
		if (!s_tx_done) {
			(void)sl_rail_idle(s_rail_handle, SL_RAIL_IDLE_ABORT, true);
		}
		(void)sl_rail_yield_radio(s_rail_handle);
		/* Restore the configurator's default fixed RX ceiling. Passing
		 * SL_RAIL_SET_FIXED_LENGTH_INVALID removes the TX override. */
		(void)sl_rail_set_fixed_length(s_rail_handle,
					       SL_RAIL_SET_FIXED_LENGTH_INVALID);

		if (repeat_interval_ms && i < repeat_cnt) {
			err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
			OSTimeDly(ms_to_ticks(repeat_interval_ms), OS_OPT_TIME_DLY, &err);
		}
	}

	/* Summary for the whole burst, not just repeat 0 -- see this
	 * function's own comment on why the i == 0-only checks above were not
	 * enough to catch a partial-burst failure. Only printed when the
	 * burst was actually large enough for a partial failure to be
	 * possible/interesting, or when any failure occurred at all, to avoid
	 * spamming RTT on every single ordinary short command.
	 */
	if (repeat_cnt > 0 || tx_fifo_fail_count || tx_start_fail_count || tx_done_timeout_count) {
		dbg_printf("radio: TX burst done, %u/%u ok (fifo fail %u, start fail %u, "
		       "TX_PACKET_SENT timeout %u)\r\n",
		       tx_ok_count, (unsigned)repeat_cnt + 1U,
		       tx_fifo_fail_count, tx_start_fail_count, tx_done_timeout_count);
	}

	s_tx_pkt_count++;
	return 0;
}

/*
 * RX frequency sweep, added after live evidence (bench pump 646910 and,
 * separately, real pump 560793) showed this radio's OOK demodulator has a
 * genuine, fairly narrow lock margin -- confirmed via a HackRF ground-truth
 * capture that caught 560793 replying with a real, correctly-decoded
 * PumpModel payload while AndroidAPS was actively retrying, at the exact
 * moment this bridge's own receiver, tuned to AndroidAPS's closest
 * candidate frequency (916.6498 MHz, ~17 kHz from the pump's independently
 * measured real reply carrier of 916.6667 MHz), reported RX_TIMING_LOST and
 * zero bytes captured. The channel filter itself is 118.56 kHz wide
 * (autogen/radioconf_generation_log.json), comfortably wide enough to pass
 * a 17 kHz offset through -- so the bottleneck is the demodulator's own
 * symbol-timing lock, not the analog filter, and there is no AFC configured
 * anywhere in this project's generated RAIL PHY to compensate for it
 * (searched; no hits). AndroidAPS's own RileyLinkTargetFrequency.kt defines
 * a fixed 50 kHz-spaced candidate grid (916.45/.50/.55/.60/.65/.70/.75/.80
 * MHz for the US/CA region) -- half that spacing, 25 kHz, is the worst case
 * gap between whatever candidate it picks and a real pump's actual carrier,
 * comfortably wider than the ~17-20 kHz margin this demodulator has already
 * been shown to need. A dedicated hardware bridge (TI CC1101-based, with
 * built-in AFC) apparently tolerates AndroidAPS's grid fine; this RAIL-based
 * OOK PHY does not, which is why this fix belongs here rather than in
 * AndroidAPS's own frequency table.
 *
 * The fix: sweep the RX-only frequency across re-arms, rather than sitting
 * on one fixed frequency for a whole listen. Offsets start at 0 (try
 * exactly what was requested first -- preserves today's fast-success
 * behaviour whenever the requested frequency is already close enough, e.g.
 * this project's own probe tooling, which empirically calibrates its own
 * target frequency first) and then alternate outward in 5 kHz steps to
 * +-25 kHz, covering the full worst-case AAPS candidate-grid gap with room
 * to spare. TX is deliberately NOT swept -- live evidence already proved TX
 * at the exact requested frequency reaches and wakes the pump (that is
 * precisely how the 560793 ground-truth reply above was obtained); only the
 * LISTEN side needs the wider net.
 *
 * REAL BUG IN THIS FIX'S OWN FIRST VERSION, caught by reading AndroidAPS's
 * actual source (RileyLinkCommunicationManager.kt's scanForDevice()) rather
 * than continuing to guess from RTT timing: the per-candidate-frequency
 * listen is NOT the ~25000 ms window this file's other diagnostics happen
 * to show elsewhere (that number comes from a different AAPS call, the
 * initial wake) -- scanForDevice() calls transmitThenReceive() with a
 * listenTimeout of exactly 1250 ms, three tries per frequency, i.e. one
 * sl_subg_get_pkt() call per try, each only long enough for about four
 * 300 ms re-arms. The first version of this sweep indexed by rearm_count,
 * which resets to 0 at the top of every sl_subg_get_pkt() call -- so every
 * single try, on every frequency, on every retry, only ever reached offsets
 * table[0..3] (0, +5000, -5000, +10000 Hz) before timing out and starting
 * over from table[0] again on the next try. +15000/+20000 Hz, right where
 * 560793's real reply actually sits relative to AndroidAPS's nearest
 * candidate, was structurally unreachable no matter how many times AAPS
 * retried. Fixed by making the sweep index a persistent static that keeps
 * advancing across calls instead of resetting -- three tries of ~4 re-arms
 * each now covers the entire 11-entry table at least once within a single
 * frequency's normal scan, and continues advancing (not resetting) if
 * AndroidAPS moves on to try a different candidate frequency next.
 *
 * TEMPORARILY DISABLED, then RE-ENABLED: config/rail/radio_settings.radioconf's
 * rx_xtal_error_ppm/tx_xtal_error_ppm were raised from 10 to 30 and the radio
 * profile regenerated (see DEBUGGING_NOTES -- 560793's own reference RFM69
 * hardware uses a hand-set 400 kHz RXBW with no ppm-budget reasoning at all;
 * this project's auto-generated OOK profile was instead sized off a 10 ppm
 * assumption, far tighter than a real pump's ~20-27 ppm-equivalent drift).
 * That alone widened Acquisition Channel Bandwidth from 118560 Hz to
 * 191840 Hz. 30 ppm is the practical ceiling here -- 35 ppm and above trips
 * "WARNING: timing window larger than max allowed 64!" from the radio
 * configurator, a real BCR timing-recovery hardware limit, not just more
 * margin, so it was not pushed further.
 *
 * The sweep was disabled on the reasoning that the wider passband alone
 * should cover the full range below -- but live testing at 30 ppm still
 * showed only a PARTIAL reception of a real 560793 reply (14 of 71 B, then
 * RX_TIMING_LOST) at a moment precisely correlated (via HackRF ground
 * truth + AndroidAPS's own logged BLE timestamps) to fall well inside the
 * expected listen window, ruling out a timing/overlap explanation. That
 * signature -- initial lock acquired, then lost partway through a longer
 * frame -- points at the BCR loop's *sustained* tracking margin, not just
 * its initial acquisition range, and residual offset within the wider
 * passband can still be large enough to matter there even though
 * acquisition itself now succeeds. Re-enabled on top of the wider filter
 * (not instead of it) on the reasoning that the two are complementary: the
 * wider passband makes every swept offset lower-risk to sit on, and
 * whichever re-arms land closest to zero residual offset during a
 * longer-frame attempt have the best chance of sustaining lock through the
 * whole frame, which a single fixed center (even now a wide-enough one)
 * cannot guarantee on its own.
 */
static const int32_t s_rx_sweep_offsets_hz[] = {
	0, 5000, -5000, 10000, -10000, 15000, -15000, 20000, -20000, 25000, -25000
};
#define SL_SUBG_RX_SWEEP_COUNT \
	(sizeof(s_rx_sweep_offsets_hz) / sizeof(s_rx_sweep_offsets_hz[0]))
/* Persists across sl_subg_get_pkt() calls deliberately -- see the comment
 * above. Not reset anywhere; free-running for the life of the firmware.
 */
static uint32_t s_rx_sweep_index;

/* Offset that actually produced the most recent successful reception, tried
 * FIRST on every subsequent call instead of always starting at a fixed
 * offset -- see sl_subg_get_pkt()'s own comment on why the first re-arm of
 * a short single-shot command is the only one with a real chance of
 * catching a freshly-woken pump's reply. A fixed guess (e.g. always 0) is
 * wrong for whichever pump/crystal combination does not happen to sit near
 * nominal -- any pump can be on any offset within this table, 646910 in
 * this session's own bench setup included; nothing here is 646910-specific
 * or guaranteed to still be true after a re-tune or a different pump.
 * Adapting to the last offset that actually worked, instead of asserting
 * one, is the only form of this that stays correct in general. Starts at 0
 * (nominal) before any success has been observed this boot, same as
 * before.
 */
static int32_t s_rx_last_good_offset_hz;

/*
 * Deliberately separate from the public sl_subg_set_freq(): that function
 * also updates s_frequency_hz, the frequency APS/AndroidAPS actually asked
 * for and which the next sl_subg_send_pkt() (a retry, or any TX at all)
 * must keep using unchanged. This helper only ever retunes the live RAIL
 * channel config for the RX side; sl_subg_get_pkt() is responsible for
 * putting the real s_frequency_hz value back before it returns by any path,
 * success or not, so a subsequent TX is never accidentally sent on a swept
 * offset. Silently no-ops (returns -1) if sl_subg_set_freq() has never run
 * yet at all -- s_frequency_channel_config_ready false -- which in practice
 * does not happen before a real listen, since every command path that
 * reaches sl_subg_get_pkt() runs apply_pending_freq() (aps.c), and thus
 * sl_subg_set_freq(), first.
 */
static int sl_subg_retune_rx_only(uint32_t hz)
{
	sl_rail_status_t st;

	if (!s_frequency_channel_config_ready) {
		return -1;
	}
	s_frequency_channel_entry.baseFrequency = hz;
	st = sl_rail_config_channels(s_rail_handle,
				     (const sl_rail_channel_config_t *)(const void *)&s_frequency_channel_config,
				     sl_rail_util_on_channel_config_change);
	return (st == SL_RAIL_STATUS_NO_ERROR) ? 0 : -1;
}

enum sl_subg_rx_status sl_subg_get_pkt(uint8_t *buf, uint8_t *len, uint32_t timeout_ms)
{
	RTOS_ERR err;
	sl_rail_status_t rail_status;
	const bool has_deadline = (timeout_ms != 0);
	OS_TICK deadline_tick = 0;
	unsigned rearm_count = 0;
	/* transaction_time sized for one full max-length frame -- fixed
	 * alongside the re-arm loop below; kept even though the loop itself
	 * turned out to be the real fix (see that comment), since it is
	 * still correct guidance for the DMP scheduler and cheap to keep.
	 *
	 * REAL BUG, found by directly comparing this against sl_subg_send_pkt()'s
	 * own scheduler_info: priority here was 200 (per sl_rail_scheduler_info_t's
	 * own documented scale, 0 highest / 255 lowest -- near the bottom), while
	 * TX uses 100. There was never a stated reason RX should be that much
	 * more preemptable than TX in the same exchange, and live evidence shows
	 * this actually costing completed receptions: with the RX filter/BCR
	 * fixes already in place, a real 560793 reply captured 14 of its 71
	 * bytes before RX_TIMING_LOST -- a partial-then-lost pattern consistent
	 * with DMP preempting an in-progress reception for a BLE connection
	 * event partway through. A 71-byte frame (~35 ms on air, this radio's
	 * own RX_FIFO threshold is 1 byte -- see sl_subg_radio_init() -- so the
	 * ISR runs on every single byte, ~71 times for one frame) has far more
	 * exposure to that than a 7-byte frame (~5 ms, a handful of ISR runs).
	 * TX, at priority 100, was directly measured at 201/201 successful
	 * completions in a real AndroidAPS wake burst (RTT: "TX burst done,
	 * 201/201 ok") -- a priority already proven not to starve BLE in
	 * practice. Matching RX to that same, already-safe value closes the
	 * asymmetry rather than guessing at a new number.
	 */
	const sl_rail_scheduler_info_t scheduler_info = {
		.priority = 100,
		.transaction_time = (((uint32_t)SL_SUBG_MAX_PKT_LEN * 8U + SL_SUBG_PREAMBLE_SYNC_BITS)
				     * 1000000U / SL_SUBG_BITRATE_BPS)
				    + SL_SUBG_RX_MARGIN_US,
	};
	/* Per-re-arm printf (one for the sweep retune, one for the RX outcome)
	 * used to fire on every single re-arm -- at SL_SUBG_RX_REARM_MS=300 ms
	 * that's ~4 re-arms for AndroidAPS's 1250 ms scan tries and ~80 for its
	 * 25000 ms wakeUp(), each producing up to two printf calls. This was
	 * live-confirmed to be enough RTT traffic to break the debug connection
	 * itself: `commander rtt connect --noreset` died with "ERROR: Could not
	 * write data to target" every single time, always specifically during
	 * this loop's printf bursts and never during quiet stretches -- ruled
	 * out as EM2/EM3 deep sleep (still happened with EM1 forced, blocking
	 * deep sleep entirely) and as a blocking printf stall (this project's
	 * IOSTREAM_RTT_UP_MODE is SEGGER_RTT_MODE_NO_BLOCK_TRIM, so printf can
	 * never block target execution regardless of host drain rate) -- but
	 * flaky RTT capture during exactly the windows this investigation most
	 * needs visibility into (e.g. the actual RX outcome during a scan's
	 * later frequencies) is a real problem on its own regardless of root
	 * cause. Fixed by accumulating outcome counters through the loop and
	 * printing one summary line per sl_subg_get_pkt() call instead, same
	 * pattern already used for sl_subg_send_pkt()'s "TX burst done" line.
	 */
	unsigned sweep_fail_count = 0;
	unsigned data_rearms = 0;
	unsigned quiet_rearms = 0;
	uint64_t error_events_accum = 0;
	uint8_t max_rx_count_seen = 0;
	int16_t last_rssi_q = INT16_MIN;

	if (has_deadline) {
		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		deadline_tick = OSTimeGet(&err) + ms_to_ticks(timeout_ms);
	}

	/*
	 * REAL BUG, CAUGHT ON LIVE HARDWARE, NOT A GUESS: this used to be one
	 * sl_rail_start_rx() call held open for the caller's entire timeout_ms
	 * (transaction_time alone, tried first, changed nothing). Live probes
	 * against the bench pump, with the RTT checkpoint below, showed the
	 * SAME signature on every attempt regardless of frequency or timeout
	 * length: ~30-36 B captured (a real reception, not silence -- no
	 * SL_RAIL_EVENT_RX_* error bits set either), then nothing further for
	 * the rest of the window, timing out with real bytes sitting in the
	 * buffer and no terminator ever found. 30-36 B is far short of even
	 * one max-length (107 B) frame. The behavior matches RAIL locking onto
	 * something -- a real signal or noise crossing the OOK threshold --
	 * and then, configured for a fixed 107 B frame with no CRC and no
	 * other end-of-frame signal, simply sitting there still "mid-packet"
	 * for the rest of the window instead of ever giving up and looking
	 * for a fresh sync word. A single long-lived RX call has no chance to
	 * recover once that happens. Periodically idling and re-arming gives
	 * a real, complete transmission (arriving at some unpredictable
	 * moment inside the listen window) many fresh chances to be the one
	 * RAIL is actually looking for a sync word during, instead of
	 * potentially just one shot that can get stuck on a false start.
	 */
	for (;;) {
		OS_FLAGS bits;
		OS_TICK wait_ticks;
		/* Set below, in the sweep-offset block; read back after a
		 * successful reception to update s_rx_last_good_offset_hz. Scoped
		 * to the whole iteration (not just that block) for that reason.
		 */
		int32_t sweep_offset_hz;

		if (has_deadline) {
			OS_TICK now;

			err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
			now = OSTimeGet(&err);
			if (now >= deadline_tick) {
				(void)sl_subg_retune_rx_only(s_frequency_hz);
				dbg_printf("radio: RX call summary: %u re-arms (%u sweep-fail, "
				       "%u data/error, %u quiet), error events 0x%016llx, "
				       "max %u B captured, last rssi %d dBm -> TIMEOUT\r\n",
				       rearm_count, sweep_fail_count, data_rearms, quiet_rearms,
				       (unsigned long long)error_events_accum, max_rx_count_seen,
				       (last_rssi_q == INT16_MIN) ? 0 : last_rssi_q / 4);
				return SL_SUBG_RX_TIMEOUT;
			}

			{
				OS_TICK remaining = deadline_tick - now;
				OS_TICK rearm_ticks = ms_to_ticks(sl_subg_rx_rearm_ms_jittered());

				wait_ticks = (remaining < rearm_ticks) ? remaining : rearm_ticks;
			}
		} else {
			/* timeout_ms == 0 means wait indefinitely -- still re-arm
			 * periodically rather than blocking on one RX call forever,
			 * for the same reason as the bounded case above.
			 */
			wait_ticks = ms_to_ticks(sl_subg_rx_rearm_ms_jittered());
		}

		s_rx_count = 0;
		s_rx_have_data = false;
		s_rx_error_events = 0;
		s_rx_error_rssi_dbm = INT16_MIN;

		/* Clear stale bits from a previous iteration/call before arming --
		 * mirrors subg_get_pkt()'s k_sem_reset(&dio1_sem) "clear any edge
		 * left over from a previous receive." Does NOT clear ABORT: an
		 * abort requested just before this call (e.g. while the preceding
		 * TX was still in flight) must still cut this receive short,
		 * exactly the ordering subg.c's own comment on NOT clearing
		 * abort_flag here depends on. aps.c's sl_subg_clear_abort() call,
		 * immediately before dispatch, is the only place that clears it.
		 */
		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		(void)OSFlagPost(&s_event_flags, SL_SUBG_EVT_RX_DATA, OS_OPT_POST_FLAG_CLR, &err);

		/*
		 * Settling delay before arming RX -- FIRST RE-ARM ONLY. Originally
		 * ran on every single re-arm; the PA ringdown / antenna-switch / AGC
		 * settling reasoning below is specifically about TX-to-RX turnaround,
		 * which only actually happens once per sl_subg_get_pkt() call (right
		 * after our own sl_subg_send_pkt() burst) -- every re-arm after the
		 * first is re-arming a radio that was already idle from the PREVIOUS
		 * re-arm, not fresh off a transmit, so the same justification does
		 * not apply there. Found while auditing this loop against
		 * AndroidAPS's own fixed, un-adjustable listen windows (scanForDevice()
		 * uses exactly 1250 ms per try -- see aps.c/DEBUGGING_NOTES): paying
		 * this 50 ms on every re-arm was costing up to ~150 ms of actual
		 * listening time out of a 1250 ms budget (3 of ~4 re-arms), and
		 * because the deadline check above happens before this delay, it was
		 * also making the whole call overrun what the caller actually asked
		 * for by the same amount, re-arm after re-arm. Skipping it after the
		 * first re-arm recovers that time without touching the original
		 * turnaround reasoning at all.
		 *
		 * PA ringdown / antenna-switch / AGC settling transient at TX-to-RX
		 * turnaround gets a chance to decay before RAIL starts trying to
		 * demodulate anything. Neither the transaction_time fix, the re-arm
		 * loop, nor the FIFO reset above changed the observed ~30-36 B
		 * phantom-capture signature at all (identical byte counts across
		 * three independent live bench-pump tests with each fix in
		 * isolation) -- ruling out DMP scheduling and stale FIFO content,
		 * and pointing at something generated fresh at arm time instead.
		 */
		if (rearm_count == 0) {
			err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
			OSTimeDly(ms_to_ticks(50U), OS_OPT_TIME_DLY, &err);
		}

		/*
		 * Also never done before this session -- the legacy RFM69 port's
		 * own subg_get_pkt() explicitly drains its FIFO immediately before
		 * every listen, with a specific documented reason: "residue from
		 * the transmit that just finished is still sitting there... a 0x00
		 * among them trips the terminator check and aborts the receive at
		 * zero length, immediately. Against AndroidAPS that looked like
		 * every 4000 ms send-and-listen completing instantly with nothing
		 * heard." This driver never had an equivalent call. The observed
		 * ~30-36 B phantom captures happen specifically on the first
		 * re-arm right after arming and never on later re-arms in the same
		 * listen (RTT-confirmed against the bench pump), which is what
		 * stale FIFO content left over from the moment RX was armed -- not
		 * a live signal -- would look like. Radio is confirmed idle here
		 * (sl_rail_idle() below on every previous iteration, and this is
		 * also the state on entry), which is this call's own documented
		 * precondition.
		 */
		(void)sl_rail_reset_fifo(s_rail_handle, false, true);

		/* RX frequency sweep -- see sl_subg_retune_rx_only()'s own comment
		 * and s_rx_sweep_index's own comment on why this index persists
		 * across calls instead of restarting at table[0] every time:
		 * AndroidAPS's real scanForDevice() gives each frequency only
		 * three 1250 ms tries (~4 re-arms each), so a per-call index would
		 * never reach this table's outer entries at all. Post-incremented
		 * so consecutive re-arms (within one call, and across separate
		 * calls) keep moving through the table rather than repeating.
		 */
		/*
		 * FIRST RE-ARM OF EVERY CALL TRIES s_rx_last_good_offset_hz --
		 * the offset that last actually worked -- INSTEAD OF drawing from
		 * the persistent sweep index. Found while investigating why 646910
		 * still failed most single-shot commands (isDeviceReachable,
		 * getBasalProfile, getPumpHistory) even right after a confirmed
		 * wake: AndroidAPS's own follow-up commands are single-shot
		 * (repeatCnt=0, no internal retry) with a short (~2 s) listen
		 * window, so only the FIRST re-arm has a real chance of catching a
		 * freshly-woken pump's immediate reply, and with the sweep
		 * unconditionally consuming the persistent index on every re-arm,
		 * that first re-arm only had a 1-in-SL_SUBG_RX_SWEEP_COUNT chance
		 * of landing on the offset this particular pump/crystal actually
		 * needs. An EARLIER version of this fix hardcoded that first re-arm
		 * to offset 0 on the reasoning that every clean 646910 decode this
		 * session had landed there -- WRONG: those decodes came from a
		 * HackRF capture at a fixed wideband center frequency, independent
		 * of this driver's own re-arm/offset state, so they say nothing
		 * about which of OUR offsets is correct, and any pump can in
		 * general sit at any offset in this table (560793 already does, at
		 * +15-20 kHz). Asserting a fixed offset is never correct in
		 * general; adapting to whichever offset last actually produced a
		 * reception is. Re-arms after the first still draw from the
		 * persistent sweep index exactly as before, so a still-unknown
		 * offset keeps getting discovered over a longer call.
		 */
		{
			int sweep_rc;

			if (rearm_count == 0) {
				sweep_offset_hz = s_rx_last_good_offset_hz;
			} else {
				sweep_offset_hz = s_rx_sweep_offsets_hz[s_rx_sweep_index % SL_SUBG_RX_SWEEP_COUNT];
				s_rx_sweep_index++;
			}
			sweep_rc = sl_subg_retune_rx_only(s_frequency_hz + (uint32_t)sweep_offset_hz);
			if (sweep_rc != 0) {
				sweep_fail_count++;
			}
		}

		rail_status = sl_rail_start_rx(s_rail_handle, s_channel, &scheduler_info);
		if (rail_status != SL_RAIL_STATUS_NO_ERROR) {
			dbg_printf("radio: start_rx failed (status 0x%04lx)\r\n",
			       (unsigned long)rail_status);
			(void)sl_subg_retune_rx_only(s_frequency_hz);
			dbg_printf("radio: RX call summary: %u re-arms (%u sweep-fail, "
			       "%u data/error, %u quiet), error events 0x%016llx, "
			       "max %u B captured, last rssi %d dBm -> TIMEOUT\r\n",
			       rearm_count, sweep_fail_count, data_rearms, quiet_rearms,
			       (unsigned long long)error_events_accum, max_rx_count_seen,
			       (last_rssi_q == INT16_MIN) ? 0 : last_rssi_q / 4);
			return SL_SUBG_RX_TIMEOUT;
		}

		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		bits = OSFlagPend(&s_event_flags, SL_SUBG_EVT_RX_DATA | SL_SUBG_EVT_ABORT,
				  wait_ticks,
				  OS_OPT_PEND_FLAG_SET_ANY | OS_OPT_PEND_BLOCKING,
				  NULL, &err);
		(void)bits;

		/*
		 * REAL BUG IN THIS SESSION'S OWN RE-ARM FIX, CAUGHT ON LIVE
		 * HARDWARE: a live test with every other fix in this function
		 * active still caught nothing at all from 646910 across ~16
		 * confirmed, HackRF-verified transmissions with zero contending
		 * traffic from any other pump in the same window -- ruling out
		 * cross-pump interference as the sole explanation and pointing
		 * back at this loop's own fixed SL_SUBG_RX_REARM_MS boundary.
		 * A real reply only takes ~62 ms on air, comfortably inside one
		 * 300 ms slice -- but only if it starts early enough in the
		 * slice. The pump's own reply timing is not synchronized to our
		 * re-arm schedule, so a reply starting late in a slice was being
		 * force-idled by sl_rail_idle() below the instant this wait
		 * timed out, mid-reception, discarding real bytes already in
		 * s_rx_buf instead of letting that specific reception finish.
		 * If the wait above genuinely timed out (not a real completion
		 * or abort) but bytes have already started arriving, give this
		 * specific reception a bounded grace period to finish before
		 * idling -- covers a reply that started anywhere in the slice
		 * without giving a truly stuck reception (the ~30-36 B signature
		 * from earlier in this investigation) more than its own fair
		 * share of extra time.
		 */
		if (RTOS_ERR_CODE_GET(err) == RTOS_ERR_TIMEOUT && s_rx_count > 0) {
			OS_TICK grace_deadline;

			err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
			grace_deadline = OSTimeGet(&err) + ms_to_ticks(SL_SUBG_RX_GRACE_MS);

			for (;;) {
				OS_TICK grace_now, grace_wait;

				err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
				grace_now = OSTimeGet(&err);
				if (grace_now >= grace_deadline) {
					break;
				}
				grace_wait = grace_deadline - grace_now;

				err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
				(void)OSFlagPend(&s_event_flags, SL_SUBG_EVT_RX_DATA | SL_SUBG_EVT_ABORT,
						 grace_wait,
						 OS_OPT_PEND_FLAG_SET_ANY | OS_OPT_PEND_BLOCKING,
						 NULL, &err);
				if (RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE) {
					break;
				}
			}
		}

		/*
		 * REAL BUG, CAUGHT WHILE ADDING THIS DIAGNOSTIC: sl_rail_get_rssi()
		 * was being called after sl_rail_idle() everywhere in this file,
		 * including the pre-existing end-of-function read that becomes
		 * s_last_rssi_dbm (the RSSI value this driver reports back to the
		 * host for every successful reply). The RAIL SDK's own doc for
		 * sl_rail_get_rssi() is explicit: "if the radio is in or
		 * transitions to IDLE or TX, SL_RAIL_RSSI_INVALID will be
		 * returned." Live-confirmed: every single reading this session,
		 * on every outcome (timing-lost failures and clean successful
		 * receptions alike), came back exactly -512 -- SL_RAIL_RSSI_INVALID
		 * in the API's own quarter-dBm units ((-128 dBm) * 4). This is
		 * almost certainly also the root cause of the "reported 0 dBm RSSI
		 * is physically implausible" issue already flagged in
		 * DEBUGGING_NOTES_2026-09-23.md from earlier probe_722_aaps.py
		 * output -- whatever downstream conversion the probe applies to a
		 * silently-invalid value happens to land on 0. Fixed by reading
		 * RSSI here, before idling, while still genuinely in RX.
		 */
		{
			int16_t rssi = sl_rail_get_rssi(s_rail_handle, SL_RAIL_GET_RSSI_NO_WAIT);

			(void)sl_rail_idle(s_rail_handle, SL_RAIL_IDLE, true);
			rearm_count++;

			if (s_rx_have_data) {
				/* sl_rail_get_rssi() returns quarter-dBm; s_last_rssi_dbm
				 * (per its own name, and rssi_to_cc111x()'s int16_t dbm
				 * parameter in aps.c) is plain dBm. The pre-existing code
				 * this replaces stored the raw quarter-dBm value directly
				 * with no conversion -- on top of always being
				 * SL_RAIL_RSSI_INVALID (-512) from the idle-timing bug,
				 * that fed rssi_to_cc111x(-512) = (-512+73)*2 = -878,
				 * which truncates to uint8_t 146 -- and AndroidAPS's own
				 * inverse formula, (146/2)-73, is exactly 0. That is
				 * almost certainly the source of the "reported 0 dBm RSSI
				 * is physically implausible" symptom already flagged in
				 * DEBUGGING_NOTES_2026-09-23.md from earlier probe output.
				 */
				s_last_rssi_dbm = rssi / 4;
			}

			/* Per-re-arm detail (byte count, error-event bitmask,
			 * error-time RSSI) folded into the accumulators below
			 * instead of printed here -- see this function's own
			 * banner comment on why per-re-arm printf was replaced
			 * with one summary line per call.
			 */
			last_rssi_q = rssi;
			error_events_accum |= s_rx_error_events;
			if (s_rx_count > max_rx_count_seen) {
				max_rx_count_seen = s_rx_count;
			}
			if (s_rx_error_events != 0 || s_rx_count != 0) {
				data_rearms++;
			} else {
				quiet_rearms++;
			}
		}

		/* s_abort_flag, not the pended bits, is the source of truth here:
		 * the ABORT bit only guarantees OSFlagPend() woke up promptly, but
		 * sl_subg_abort() may have been called and cleared again (by a
		 * second sl_subg_clear_abort()) before this line runs on a slow
		 * scheduler -- s_abort_flag reflects the flag's value at THIS
		 * instant, same contract subg_get_pkt() had with abort_flag.
		 */
		if (s_abort_flag) {
			(void)sl_subg_retune_rx_only(s_frequency_hz);
			dbg_printf("radio: RX call summary: %u re-arms (%u sweep-fail, "
			       "%u data/error, %u quiet), error events 0x%016llx, "
			       "max %u B captured, last rssi %d dBm -> INTERRUPTED\r\n",
			       rearm_count, sweep_fail_count, data_rearms, quiet_rearms,
			       (unsigned long long)error_events_accum, max_rx_count_seen,
			       (last_rssi_q == INT16_MIN) ? 0 : last_rssi_q / 4);
			return SL_SUBG_RX_INTERRUPTED;
		}
		if (s_rx_have_data && s_rx_count != 0) {
			/* Remember the offset this reception actually happened on, so
			 * the next call's first (highest-value) re-arm starts here
			 * instead of guessing -- see s_rx_last_good_offset_hz's own
			 * comment.
			 */
			s_rx_last_good_offset_hz = sweep_offset_hz;
			break;
		}
		/* Nothing usable this iteration -- loop back, re-check the
		 * deadline, and re-arm for another slice.
		 */
	}

	/* Sweep restored to the actual requested frequency now that a real
	 * reception ended this listen -- see sl_subg_retune_rx_only()'s own
	 * comment. Harmless if this particular successful re-arm happened to
	 * land on offset 0 already (retuning to the same frequency again is a
	 * no-op as far as the radio is concerned).
	 */
	(void)sl_subg_retune_rx_only(s_frequency_hz);

	/* s_last_rssi_dbm was already captured above, before this successful
	 * iteration's sl_rail_idle() call -- see that block's own comment for
	 * why reading it here (after the loop, radio long since idle) would
	 * only ever return SL_RAIL_RSSI_INVALID.
	 */
	memcpy(buf, s_rx_buf, s_rx_count);
	*len = s_rx_count;
	s_rx_pkt_count++;
	dbg_printf("radio: RX call summary: %u re-arms (%u sweep-fail, %u data/error, "
	       "%u quiet), error events 0x%016llx, max %u B captured, "
	       "last rssi %d dBm -> OK, %u B\r\n",
	       rearm_count, sweep_fail_count, data_rearms, quiet_rearms,
	       (unsigned long long)error_events_accum, max_rx_count_seen,
	       (last_rssi_q == INT16_MIN) ? 0 : last_rssi_q / 4, s_rx_count);
	return SL_SUBG_RX_OK;
}

void sl_subg_abort(void)
{
	RTOS_ERR err;

	s_abort_flag = true;
	/* Wakes a blocked sl_subg_get_pkt() immediately rather than leaving it to
	 * run out its full timeout -- the entire point of the source's
	 * Subg_SetIntFlg()/DIO1-adjacent design (see subg.c's own extensive note
	 * on why a level-vs-edge race made a bounded polling fallback necessary
	 * there; Micrium OS event flags have no such race, so no fallback poll
	 * interval is needed here).
	 */
	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
	(void)OSFlagPost(&s_event_flags, SL_SUBG_EVT_ABORT, OS_OPT_POST_FLAG_SET, &err);
}

void sl_subg_clear_abort(void)
{
	RTOS_ERR err;

	s_abort_flag = false;
	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
	(void)OSFlagPost(&s_event_flags, SL_SUBG_EVT_ABORT, OS_OPT_POST_FLAG_CLR, &err);
}

int16_t sl_subg_get_last_rssi(void)
{
	return s_last_rssi_dbm;
}

int sl_subg_set_power_level(int16_t dbm)
{
	/* sl_rail_set_tx_power_dbm() takes DECI-dBm (sl_rail_types.h: typedef
	 * int16_t sl_rail_tx_power_t) -- see the header note on this function for
	 * the RFM69-driver contrast (raw PA register value there, whole dBm here).
	 */
	sl_rail_status_t st = sl_rail_set_tx_power_dbm(s_rail_handle, (sl_rail_tx_power_t)(dbm * 10));

	return (st == SL_STATUS_OK) ? 0 : -1;
}

uint16_t sl_subg_get_rx_count(void)
{
	return s_rx_pkt_count;
}

uint16_t sl_subg_get_tx_count(void)
{
	return s_tx_pkt_count;
}

int sl_subg_set_freq(uint32_t hz)
{
	const RAIL_ChannelConfig_t *generated_config;
	sl_rail_status_t st;

	if (hz < SL_SUBG_FREQ_MIN_HZ || hz > SL_SUBG_FREQ_MAX_HZ) {
		return -1;
	}

	if (!s_frequency_channel_config_ready) {
		generated_config = channelConfigs[0];
		if (generated_config == NULL || generated_config->length == 0
		    || generated_config->configs == NULL) {
			return -1;
		}

		s_frequency_channel_config = *generated_config;
		s_frequency_channel_entry = generated_config->configs[0];
		s_frequency_channel_config.configs = &s_frequency_channel_entry;
		s_frequency_channel_config.length = 1;
		s_frequency_channel_entry.channelSpacing = 0;
		s_frequency_channel_entry.physicalChannelOffset = 0;
		s_frequency_channel_entry.channelNumberStart = 0;
		s_frequency_channel_entry.channelNumberEnd = 0;
		s_frequency_channel_config_ready = true;
	}

	/* A zero channel spacing maps logical channel 0 directly to baseFrequency.
	 * Re-register the same static config after changing the requested frequency;
	 * RAIL applies it on the next start_tx/start_rx call.
	 */
	s_frequency_channel_entry.baseFrequency = hz;
	st = sl_rail_config_channels(s_rail_handle,
				     (const sl_rail_channel_config_t *)(const void *)&s_frequency_channel_config,
				     sl_rail_util_on_channel_config_change);
	if (st != SL_RAIL_STATUS_NO_ERROR) {
		return -1;
	}

	s_channel = 0;
	s_frequency_hz = hz;
	return 0;
}

uint32_t sl_subg_get_freq(void)
{
	sl_rail_channel_metadata_t metadata;
	uint16_t count = 1;

	if (s_rail_handle != SL_RAIL_EFR32_HANDLE
	    && sl_rail_get_channel_metadata(s_rail_handle, &metadata, &count,
					    s_channel, s_channel) == SL_RAIL_STATUS_NO_ERROR
	    && count == 1) {
		return metadata.frequency_hz;
	}

	return s_frequency_hz;
}

void sl_subg_reset_radio_cfg(void)
{
	(void)sl_subg_set_freq(SL_SUBG_FREQ_HZ);
}
