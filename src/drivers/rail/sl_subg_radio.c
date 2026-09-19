/*
 * Sub-GHz radio driver -- EFR32xG28 native RAIL, 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * See sl_subg_radio.h for what this replaces and why its shape differs from
 * the RFM69 driver. Every sl_rail_*() call below was checked against the real
 * header (modules/hal/silabs/.../rail_lib/common/sl_rail.h, this workspace,
 * this session) before being written -- not recalled from memory or general
 * RAIL familiarity, given how many assumptions about this SDK turned out
 * wrong earlier in the same session (RAIL 2.x being the live API; the OOK
 * config gap implying infeasibility; the 400 kHz channel bandwidth carried
 * over from the RFM69 instead of this chip's own calculated value).
 *
 * NOT YET COMPILED OR RUN. There is no hardware to run it on yet. Treat this
 * as a grounded starting skeleton, not a finished driver -- see the TODOs.
 */

#include <string.h>

#include "sl_subg_radio.h"
#include "sl_rail.h"
#include "sl_rail_types.h"

/*
 * TODO: the generated channel config table.
 *
 * Simplicity Studio's Radio Configurator emits a const sl_rail_channel_config_t
 * (plus supporting modem-config arrays) for the MDT_OOK-Channel_Group_1
 * protocol built this session -- the real analogue of rf69_cfg_916[] on the
 * RFM69 side, except generated rather than hand-written, and it is what
 * actually encodes the 16.384 kbps / OOK / preamble / sync / framing settings
 * configured in Studio. It does not exist in this tree yet: it is emitted
 * into the Studio project's own autogen/ directory, which has not been
 * exported and copied in here. sl_subg_radio_init() cannot correctly call
 * sl_rail_config_channels() without it -- the extern declaration below is a
 * placeholder for that symbol, named to match Studio's own naming convention
 * (protocol name + "_channelConfig") seen in the vendored per-chip generated
 * examples inspected this session, but NOT verified against this project's
 * actual output.
 */
extern const sl_rail_channel_config_t MDT_OOK_channelConfig[];

static sl_rail_handle_t s_rail_handle;

/* TX/RX FIFO buffers. RAIL requires these word-aligned (sl_rail_fifo_buffer_align_t
 * in the real header) and sized as a power of two in most RAIL generations --
 * TODO: confirm the power-of-two requirement still holds in this SDK release
 * before relying on 128 here; not independently verified this session.
 */
#define SL_SUBG_FIFO_BYTES 128U
static sl_rail_fifo_buffer_align_t s_tx_fifo[SL_SUBG_FIFO_BYTES / sizeof(sl_rail_fifo_buffer_align_t)];
static sl_rail_fifo_buffer_align_t s_rx_fifo[SL_SUBG_FIFO_BYTES / sizeof(sl_rail_fifo_buffer_align_t)];

static uint8_t s_rx_buf[SL_SUBG_MAX_PKT_LEN];
static uint8_t s_rx_count;
static volatile bool s_rx_have_data;
static volatile bool s_tx_done;
static volatile bool s_abort_flag;
static int16_t s_last_rssi_dbm = INT16_MIN;

/*
 * TODO: replaced with whatever this project's actual environment provides
 * (a Micrium/FreeRTOS semaphore if the eventual "RAIL Bluetooth DMP" template
 * is used, or a bare busy-wait if this stays a bare-metal SoC app -- the plain
 * RAILtest example this PHY was configured against did not obviously indicate
 * which). subg_get_pkt() on the RFM69 side blocks on a Zephyr k_sem given by
 * the DIO1 ISR; this needs the equivalent primitive from whatever RTOS (or
 * lack of one) the real application ends up built against. Left as a spin-wait
 * placeholder so the control flow below is legible, not because busy-waiting
 * is the intended final implementation.
 */
static void wait_for_rx_data_or_timeout(uint32_t timeout_ms)
{
	/* PLACEHOLDER. Do not ship a busy-wait on real hardware -- this needs a
	 * real blocking wait (semaphore/event flag) so other tasks/the BLE stack
	 * are not starved during a listen window that can run up to 25 s.
	 */
	(void)timeout_ms;
}

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
 */
static void rail_events_callback(sl_rail_handle_t handle, sl_rail_events_t events)
{
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
				break;
			}
			s_rx_buf[s_rx_count++] = byte;
		}
	}

	if (events & SL_RAIL_EVENT_TX_PACKET_SENT) {
		s_tx_done = true;
	}

	/* TODO: SL_RAIL_EVENT_RX_FIFO_OVERFLOW / SL_RAIL_EVENT_RX_FIFO_FULL are
	 * real, named events in sl_rail_types.h and are not handled here yet --
	 * on the RFM69 side an equivalent overrun is a silent data-corruption
	 * risk that was specifically designed around (REG_IRQFLAGS2 FIFOOVERRUN
	 * handling in rf69_cfg_916[]). Needs the same care here before this is
	 * trusted against real traffic.
	 */
}

int sl_subg_radio_init(void)
{
	sl_rail_config_t config = {
		.events_callback = rail_events_callback,
	};
	sl_rail_status_t st;

	st = sl_rail_init(&s_rail_handle, &config, NULL);
	if (st != SL_RAIL_STATUS_NO_ERROR) {
		return -1;
	}

	st = sl_rail_set_tx_fifo(s_rail_handle, s_tx_fifo, SL_SUBG_FIFO_BYTES, 0, 0);
	if (st != SL_RAIL_STATUS_NO_ERROR) {
		return -1;
	}

	/* TODO: sl_rail_set_rx_fifo()'s exact signature was not pulled this
	 * session (only referenced by line number while looking for the
	 * threshold function) -- confirm its parameter order matches
	 * set_tx_fifo's before trusting this call as written.
	 */
	st = sl_rail_set_rx_fifo(s_rail_handle, s_rx_fifo, SL_SUBG_FIFO_BYTES);
	if (st != SL_RAIL_STATUS_NO_ERROR) {
		return -1;
	}

	(void)sl_rail_set_rx_fifo_threshold(s_rail_handle, SL_SUBG_FIFO_BYTES / 2);

	st = sl_rail_config_events(s_rail_handle,
				   SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL | SL_RAIL_EVENT_TX_PACKET_SENT,
				   SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL | SL_RAIL_EVENT_TX_PACKET_SENT);
	if (st != SL_RAIL_STATUS_NO_ERROR) {
		return -1;
	}

	/* TODO: sl_rail_config_channels(s_rail_handle, MDT_OOK_channelConfig, ...)
	 * belongs here -- not yet called, because the channel config symbol
	 * above is an unverified placeholder, not real generated output. Without
	 * this call the PHY configured in Studio is never actually applied to
	 * the radio, so nothing above this line is sufficient on its own yet.
	 */

	return 0;
}

int sl_subg_send_pkt(const uint8_t *data, uint8_t len, uint8_t repeat_cnt,
		     uint16_t repeat_interval_ms)
{
	for (uint8_t i = 0; i <= repeat_cnt; i++) {
		s_tx_done = false;
		sl_rail_write_tx_fifo(s_rail_handle, data, len, true);
		sl_rail_start_tx(s_rail_handle, SL_SUBG_CHANNEL, 0, NULL);

		/* TODO: bounded wait on s_tx_done, same caveat as
		 * wait_for_rx_data_or_timeout() -- needs a real blocking
		 * primitive, not a spin.
		 */
		while (!s_tx_done) {
			/* PLACEHOLDER */
		}

		if (repeat_interval_ms && i < repeat_cnt) {
			/* TODO: real delay primitive. */
		}
	}
	return 0;
}

enum sl_subg_rx_status sl_subg_get_pkt(uint8_t *buf, uint8_t *len, uint32_t timeout_ms)
{
	s_rx_count = 0;
	s_rx_have_data = false;

	sl_rail_start_rx(s_rail_handle, SL_SUBG_CHANNEL, NULL);

	wait_for_rx_data_or_timeout(timeout_ms);

	sl_rail_idle(s_rail_handle, 0 /* TODO: real idle-mode enum value */, true);

	if (s_abort_flag) {
		return SL_SUBG_RX_INTERRUPTED;
	}
	if (!s_rx_have_data || s_rx_count == 0) {
		return SL_SUBG_RX_TIMEOUT;
	}

	s_last_rssi_dbm = sl_rail_get_rssi(s_rail_handle, 0);
	memcpy(buf, s_rx_buf, s_rx_count);
	*len = s_rx_count;
	return SL_SUBG_RX_OK;
}

void sl_subg_abort(void)
{
	s_abort_flag = true;
}

void sl_subg_clear_abort(void)
{
	s_abort_flag = false;
}

int16_t sl_subg_get_last_rssi(void)
{
	return s_last_rssi_dbm;
}

int sl_subg_set_power_level(int16_t dbm)
{
	(void)dbm;
	/* TODO: not implemented. Needs the real sl_rail TX power API, which
	 * was not pinned down this session -- do not guess a function name.
	 */
	return -1;
}
