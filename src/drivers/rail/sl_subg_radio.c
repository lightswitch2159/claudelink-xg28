/*
 * Sub-GHz radio driver -- EFR32xG28 native RAIL, 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * See sl_subg_radio.h for what this replaces, why its shape differs from the
 * RFM69 driver, and the API-family correction made before this rewrite (RAIL_*,
 * not sl_rail_* -- the "deprecated" PascalCase functions are what Silicon
 * Labs' own Radio Configurator output and generated init glue actually use
 * for this project).
 *
 * Every RAIL_*() call below was checked against the real header
 * (modules/hal/silabs/.../rail_lib/common/rail.h, this workspace) or against
 * Silicon Labs' own generated code in
 * SimplicityStudio/v6_workspace/rail_soc_railtest/autogen/, not recalled from
 * memory.
 *
 * Verified to compile clean (zero errors, zero warnings with -Wall -Wextra)
 * against the real project's exact toolchain, include paths, and defines --
 * see tools/check_compile.sh, which extracts that configuration directly
 * from the live generated project rather than hand-maintaining a copy. NOT
 * YET RUN: there is no hardware to run it on. Treat this as a compile-clean
 * starting skeleton, not a hardware-validated driver -- see the TODOs.
 */

#include <string.h>

#include "sl_subg_radio.h"
#include "rail.h"
#include "rail_types.h"
#include "sl_rail_util_init.h"   /* sl_rail_util_init(), sl_rail_util_get_handle() */

static RAIL_Handle_t s_rail_handle;

/* TX/RX FIFO buffers. RAIL_SetTxFifo/RAIL_SetRxFifo take plain uint8_t*
 * buffers (confirmed from the real signatures, unlike the sl_rail_* family's
 * word-aligned sl_rail_fifo_buffer_align_t type this file previously assumed
 * by analogy rather than checking directly for the RAIL_* equivalent).
 */
#define SL_SUBG_FIFO_BYTES 128U
static uint8_t s_tx_fifo[SL_SUBG_FIFO_BYTES];
static uint8_t s_rx_fifo[SL_SUBG_FIFO_BYTES];

static uint8_t s_rx_buf[SL_SUBG_MAX_PKT_LEN];
static uint8_t s_rx_count;
static volatile bool s_rx_have_data;
static volatile bool s_tx_done;
static volatile bool s_abort_flag;
static int16_t s_last_rssi_dbm = INT16_MIN;
static uint16_t s_rx_pkt_count;
static uint16_t s_tx_pkt_count;
static uint16_t s_channel = SL_SUBG_CHANNEL;

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
 * RAIL_EVENT_RX_FIFO_ALMOST_FULL is the direct analogue of DIO1 mapped to
 * FifoNotEmpty: it fires as bytes become available, before the packet (by our
 * own fixed-length declaration, up to 107 bytes) is complete. Draining here
 * and watching for the 0x00 terminator reproduces subg_get_pkt()'s existing
 * "terminate on a sentinel byte, do not wait for the radio's own notion of
 * packet-complete" behaviour.
 *
 * WIRING, RESOLVED: this function is deliberately named sl_rail_util_on_event
 * and given external (non-static) linkage to override a __WEAK stub of the
 * same name in the generated autogen/sl_rail_util_callbacks.c -- confirmed by
 * reading that file directly. Its own internal sli_rail_util_on_event(),
 * which IS what sl_rail_util_init() wires into RAIL_Config_t::eventsCallback,
 * does nothing but call sl_rail_util_on_event(); the weak default is an empty
 * stub, and that generated file's own header warns "any application code
 * placed within this file will be discarged upon project regeneration" --
 * i.e. the intended pattern really is a weak-symbol override living outside
 * autogen/, not editing the generated file. No manual callback chaining or
 * re-arming needed. RAIL_ConfigEvents() in sl_subg_radio_init() below is
 * still required, separately -- it controls which event bits are unmasked at
 * the radio, not which function receives them.
 */
void sl_rail_util_on_event(RAIL_Handle_t handle, RAIL_Events_t events)
{
	if (events & RAIL_EVENT_RX_FIFO_ALMOST_FULL) {
		while (s_rx_count < SL_SUBG_MAX_PKT_LEN) {
			uint8_t byte;
			uint16_t got = RAIL_ReadRxFifo(handle, &byte, 1);

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

	if (events & RAIL_EVENT_TX_PACKET_SENT) {
		s_tx_done = true;
	}

	/* TODO: RAIL_EVENT_RX_FIFO_OVERFLOW is a real, named event and is not
	 * handled here yet -- on the RFM69 side an equivalent overrun is a
	 * silent data-corruption risk that was specifically designed around
	 * (REG_IRQFLAGS2 FIFOOVERRUN handling in rf69_cfg_916[]). Needs the same
	 * care here before this is trusted against real traffic.
	 */
}

int sl_subg_radio_init(void)
{
	uint16_t rx_size = SL_SUBG_FIFO_BYTES;
	uint16_t got;
	RAIL_Status_t st;

	/*
	 * Bring RAIL up via Silicon Labs' own generated bring-up, not a hand
	 * rolled RAIL_Init()/RAIL_ConfigChannels() pair -- it is already wired to
	 * the real MDT_OOK channel config (autogen/rail_config.c) and to
	 * calibration/PA setup this driver has no reason to reimplement.
	 */
	/*
	 * sl_rail_util_init() already APP_ASSERT()s internally on failure (seen
	 * in the generated sl_rail_util_init_inst0(): "RAIL_Init failed" halts
	 * execution rather than returning), so a real failure would not reach
	 * this line at all -- this check is defensive, not a confirmed way to
	 * detect failure. RAIL_EFR32_HANDLE is documented as a pre-init
	 * placeholder value, not a guaranteed post-init failure sentinel, so
	 * treat this comparison as a best-effort guard, not a verified contract.
	 */
	sl_rail_util_init();
	s_rail_handle = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
	if (s_rail_handle == RAIL_EFR32_HANDLE) {
		return -1;
	}

	got = RAIL_SetTxFifo(s_rail_handle, s_tx_fifo, 0, SL_SUBG_FIFO_BYTES);
	if (got != SL_SUBG_FIFO_BYTES) {
		return -1;
	}

	st = RAIL_SetRxFifo(s_rail_handle, s_rx_fifo, &rx_size);
	if (st != RAIL_STATUS_NO_ERROR) {
		return -1;
	}

	(void)RAIL_SetRxFifoThreshold(s_rail_handle, SL_SUBG_FIFO_BYTES / 2);

	st = RAIL_ConfigEvents(s_rail_handle,
			       RAIL_EVENT_RX_FIFO_ALMOST_FULL | RAIL_EVENT_TX_PACKET_SENT,
			       RAIL_EVENT_RX_FIFO_ALMOST_FULL | RAIL_EVENT_TX_PACKET_SENT);
	if (st != RAIL_STATUS_NO_ERROR) {
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
	for (uint8_t i = 0; i <= repeat_cnt; i++) {
		s_tx_done = false;
		RAIL_WriteTxFifo(s_rail_handle, data, len, true);
		RAIL_StartTx(s_rail_handle, s_channel, RAIL_TX_OPTIONS_DEFAULT, NULL);

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
	s_tx_pkt_count++;
	return 0;
}

enum sl_subg_rx_status sl_subg_get_pkt(uint8_t *buf, uint8_t *len, uint32_t timeout_ms)
{
	s_rx_count = 0;
	s_rx_have_data = false;

	RAIL_StartRx(s_rail_handle, s_channel, NULL);

	wait_for_rx_data_or_timeout(timeout_ms);

	RAIL_Idle(s_rail_handle, RAIL_IDLE, true);

	if (s_abort_flag) {
		return SL_SUBG_RX_INTERRUPTED;
	}
	if (!s_rx_have_data || s_rx_count == 0) {
		return SL_SUBG_RX_TIMEOUT;
	}

	s_last_rssi_dbm = RAIL_GetRssi(s_rail_handle, false);
	memcpy(buf, s_rx_buf, s_rx_count);
	*len = s_rx_count;
	s_rx_pkt_count++;
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
	/* RAIL_SetTxPowerDbm() takes DECI-dBm (rail.h doc example: "100 deci-dBm,
	 * 10 dBm") -- see the header note on this function for the RFM69-driver
	 * contrast (raw PA register value there, whole dBm here).
	 */
	RAIL_Status_t st = RAIL_SetTxPowerDbm(s_rail_handle, (RAIL_TxPower_t)(dbm * 10));

	return (st == RAIL_STATUS_NO_ERROR) ? 0 : -1;
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
	if (hz < SL_SUBG_BASE_FREQ_HZ) {
		return -1;
	}

	uint32_t n = (hz - SL_SUBG_BASE_FREQ_HZ + SL_SUBG_CHANNEL_SPACING_HZ / 2)
		     / SL_SUBG_CHANNEL_SPACING_HZ;

	/* SL_SUBG_CHANNEL_MIN is 0, so n (unsigned) is never below it -- only the
	 * upper bound is a real check.
	 */
	if (n > SL_SUBG_CHANNEL_MAX) {
		return -1;
	}

	s_channel = (uint16_t)n;
	return 0;
}

uint32_t sl_subg_get_freq(void)
{
	return SL_SUBG_BASE_FREQ_HZ + (uint32_t)s_channel * SL_SUBG_CHANNEL_SPACING_HZ;
}

void sl_subg_reset_radio_cfg(void)
{
	s_channel = SL_SUBG_CHANNEL;
}
