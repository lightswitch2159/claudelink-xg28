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

	if (events & SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL) {
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

	/* TODO: SL_RAIL_EVENT_RX_FIFO_OVERFLOW is a real, named event and is not
	 * handled here yet -- on the RFM69 side an equivalent overrun is a
	 * silent data-corruption risk that was specifically designed around
	 * (REG_IRQFLAGS2 FIFOOVERRUN handling in rf69_cfg_916[]). Needs the same
	 * care here before this is trusted against real traffic.
	 */
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
	(void)sl_rail_set_rx_fifo_threshold(s_rail_handle, 1U);

	st = sl_rail_config_events(s_rail_handle,
				   SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL | SL_RAIL_EVENT_TX_PACKET_SENT
				   | SL_RAIL_EVENT_TX_UNDERFLOW,
				   SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL | SL_RAIL_EVENT_TX_PACKET_SENT
				   | SL_RAIL_EVENT_TX_UNDERFLOW);
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
	OS_FLAGS bits;
	sl_rail_status_t rail_status;
	/* The receive is an indefinite RAIL operation, so transaction_time is
	 * intentionally omitted. Priority 200 follows Silicon Labs' DMP example
	 * for background RX; the scheduler can still interleave higher-priority
	 * BLE connection events on the shared radio.
	 */
	const sl_rail_scheduler_info_t scheduler_info = {
		.priority = 200,
	};

	s_rx_count = 0;
	s_rx_have_data = false;

	/* Clear stale bits from a previous call before arming -- mirrors
	 * subg_get_pkt()'s k_sem_reset(&dio1_sem) "clear any edge left over from a
	 * previous receive." Does NOT clear ABORT: an abort requested just before
	 * this call (e.g. while the preceding TX was still in flight) must still
	 * cut this receive short, exactly the ordering subg.c's own comment on
	 * NOT clearing abort_flag here depends on. aps.c's sl_subg_clear_abort()
	 * call, immediately before dispatch, is the only place that clears it.
	 */
	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
	(void)OSFlagPost(&s_event_flags, SL_SUBG_EVT_RX_DATA, OS_OPT_POST_FLAG_CLR, &err);

	rail_status = sl_rail_start_rx(s_rail_handle, s_channel, &scheduler_info);
	if (rail_status != SL_RAIL_STATUS_NO_ERROR) {
		printf("radio: start_rx failed (status 0x%04lx)\r\n",
		       (unsigned long)rail_status);
		return SL_SUBG_RX_TIMEOUT;
	}

	/* timeout_ms == 0 means wait indefinitely -- see ms_to_ticks() above for
	 * why 0 maps straight through to OSFlagPend()'s own 0-means-forever
	 * convention rather than needing a sentinel translation.
	 */
	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
	bits = OSFlagPend(&s_event_flags, SL_SUBG_EVT_RX_DATA | SL_SUBG_EVT_ABORT,
			  ms_to_ticks(timeout_ms),
			  OS_OPT_PEND_FLAG_SET_ANY | OS_OPT_PEND_BLOCKING,
			  NULL, &err);
	(void)bits;

	(void)sl_rail_idle(s_rail_handle, SL_RAIL_IDLE, true);

	/* s_abort_flag, not the pended bits, is the source of truth here: the
	 * ABORT bit only guarantees OSFlagPend() woke up promptly, but
	 * sl_subg_abort() may have been called and cleared again (by a second
	 * sl_subg_clear_abort()) before this line runs on a slow scheduler --
	 * s_abort_flag reflects the flag's value at THIS instant, same contract
	 * subg_get_pkt() had with abort_flag.
	 */
	if (s_abort_flag) {
		return SL_SUBG_RX_INTERRUPTED;
	}
	if (!s_rx_have_data || s_rx_count == 0) {
		return SL_SUBG_RX_TIMEOUT;
	}

	s_last_rssi_dbm = sl_rail_get_rssi(s_rail_handle, 0);
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
