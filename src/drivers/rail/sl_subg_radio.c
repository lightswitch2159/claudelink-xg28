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
 */
#define SL_SUBG_RX_REARM_MS           300U
/* Grace period granted to an in-progress reception (bytes already
 * arriving, no terminator yet) when the re-arm timer above expires before
 * it finishes -- see sl_subg_get_pkt()'s own comment for why. Comfortably
 * longer than one full max-length frame (~62 ms) past whatever portion of
 * the 300 ms slice was already spent waiting.
 */
#define SL_SUBG_RX_GRACE_MS           150U

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
		printf("radio: RX FIFO mode config failed (status 0x%04lx)\r\n",
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
		printf("radio: failed to set TX power to +13 dBm\r\n");
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
			printf("radio: RX FIFO threshold got %u, not 1\r\n", applied);
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

	return 0;
}

int sl_subg_send_pkt(const uint8_t *data, uint8_t len, uint8_t repeat_cnt,
		     uint16_t repeat_interval_ms)
{
	RTOS_ERR err;

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
			printf("radio: failed to set TX frame length %u (got %u)\r\n",
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

		if (i == 0 && (fifo_written != tx_frame_len || tx_sc != SL_RAIL_STATUS_NO_ERROR)) {
			/* Permanent checkpoint, not temporary -- see the comment
			 * above. First repeat only: repeat_cnt can be 200+ (a
			 * real AndroidAPS wakeup burst), and a genuine failure
			 * here is systemic, not a one-off worth repeating 200
			 * times over RTT.
			 */
			printf("radio: TX FAILED (fifo %u/%u B written, start_tx status 0x%04lx)\r\n",
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

		if (i == 0 && !s_tx_done) {
			printf("radio: TX_PACKET_SENT timeout%s\r\n",
			       s_tx_underflow ? " after TX_UNDERFLOW" : "");
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
	s_tx_pkt_count++;
	return 0;
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
	 */
	const sl_rail_scheduler_info_t scheduler_info = {
		.priority = 200,
		.transaction_time = (((uint32_t)SL_SUBG_MAX_PKT_LEN * 8U + SL_SUBG_PREAMBLE_SYNC_BITS)
				     * 1000000U / SL_SUBG_BITRATE_BPS)
				    + SL_SUBG_RX_MARGIN_US,
	};

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

		if (has_deadline) {
			OS_TICK now;

			err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
			now = OSTimeGet(&err);
			if (now >= deadline_tick) {
				return SL_SUBG_RX_TIMEOUT;
			}

			{
				OS_TICK remaining = deadline_tick - now;
				OS_TICK rearm_ticks = ms_to_ticks(SL_SUBG_RX_REARM_MS);

				wait_ticks = (remaining < rearm_ticks) ? remaining : rearm_ticks;
			}
		} else {
			/* timeout_ms == 0 means wait indefinitely -- still re-arm
			 * periodically rather than blocking on one RX call forever,
			 * for the same reason as the bounded case above.
			 */
			wait_ticks = ms_to_ticks(SL_SUBG_RX_REARM_MS);
		}

		s_rx_count = 0;
		s_rx_have_data = false;
		s_rx_error_events = 0;

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
		 * Settling delay before arming RX. Every single call into this
		 * function in this protocol is immediately preceded by our own
		 * sl_subg_send_pkt() burst -- CMD_SEND_AND_LISTEN transmits, then
		 * listens, by design -- so a PA ringdown / antenna-switch / AGC
		 * settling transient at TX-to-RX turnaround gets a chance to decay
		 * before RAIL starts trying to demodulate anything. Neither the
		 * transaction_time fix, the re-arm loop, nor the FIFO reset above
		 * changed the observed ~30-36 B phantom-capture signature at all
		 * (identical byte counts across three independent live bench-pump
		 * tests with each fix in isolation) -- ruling out DMP scheduling
		 * and stale FIFO content, and pointing at something generated
		 * fresh at arm time instead. This is the next candidate; not yet
		 * confirmed.
		 */
		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		OSTimeDly(ms_to_ticks(50U), OS_OPT_TIME_DLY, &err);

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

		rail_status = sl_rail_start_rx(s_rail_handle, s_channel, &scheduler_info);
		if (rail_status != SL_RAIL_STATUS_NO_ERROR) {
			printf("radio: start_rx failed (status 0x%04lx)\r\n",
			       (unsigned long)rail_status);
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

			/* Logged here (task context, safe to printf) rather than
			 * from sl_rail_util_on_event() (ISR context, where repeated
			 * printf during an actual incoming packet could itself
			 * perturb the timing this is trying to diagnose). A nonzero
			 * error-events value with a truncated byte count is the DMP
			 * protocol-switch signature (see s_rx_error_events' own
			 * declaration); this now also logs which re-arm iteration
			 * produced it, since that distinguishes "stuck on the very
			 * first attempt" from "intermittently stuck partway through
			 * the listen."
			 *
			 * RSSI logged while gathering real calibration data for a
			 * future RSSI/carrier-sense based termination scheme (see
			 * DEBUGGING_NOTES_2026-09-23.md Follow-up 6's "Next steps").
			 * Logged on every re-arm, not just ones with captured bytes,
			 * specifically to also capture quiet-channel/noise-floor
			 * baseline samples for comparison -- a real squelch
			 * threshold needs both. Values are quarter-dBm (divide by 4
			 * for dBm) per sl_rail_get_rssi()'s own documented units.
			 */
			if (s_rx_error_events != 0 || s_rx_count != 0) {
				printf("radio: RX re-arm %u ended with %u B captured, "
				       "error events 0x%016llx, rssi %d (%d dBm)\r\n",
				       rearm_count, s_rx_count,
				       (unsigned long long)s_rx_error_events, rssi, rssi / 4);
			} else if ((rearm_count % 20U) == 1U) {
				/* Sparse baseline sample -- every 20th empty re-arm
				 * (roughly every 6 s at the 300 ms interval), not
				 * every single one, to avoid RTT spam over a long
				 * listen window while still getting real noise-floor
				 * data points across the whole session.
				 */
				printf("radio: RX re-arm %u quiet, rssi %d (%d dBm)\r\n",
				       rearm_count, rssi, rssi / 4);
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
			return SL_SUBG_RX_INTERRUPTED;
		}
		if (s_rx_have_data && s_rx_count != 0) {
			break;
		}
		/* Nothing usable this iteration -- loop back, re-check the
		 * deadline, and re-arm for another slice.
		 */
	}

	/* s_last_rssi_dbm was already captured above, before this successful
	 * iteration's sl_rail_idle() call -- see that block's own comment for
	 * why reading it here (after the loop, radio long since idle) would
	 * only ever return SL_RAIL_RSSI_INVALID.
	 */
	memcpy(buf, s_rx_buf, s_rx_count);
	*len = s_rx_count;
	s_rx_pkt_count++;
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
