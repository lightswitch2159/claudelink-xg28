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
 *
 * RTOS, RESOLVED THIS SESSION: FreeRTOS. Silicon Labs ships exactly two DMP
 * (Bluetooth + proprietary RAIL) project templates that actually coexist BLE
 * with a RAIL protocol -- "Bluetooth RAIL DMP - SoC Empty" and "...- SoC
 * Light" -- and BOTH require an RTOS (a FreeRTOS variant and a Micrium OS
 * variant of each; there is no bare-metal DMP template in this SDK at all).
 * Confirmed by reading the real .slcp manifests and example source under
 * ~/.silabs/slt/installs/conan/p/simpleca33d691c539/p/bluetooth_le_app/
 * example/bt_rail_dmp_soc_empty/, not inferred. FreeRTOS chosen over Micrium
 * OS as the more portable/open option; nothing here depends on that specific
 * choice being final. This resolves the two busy-spin placeholders below --
 * see the WAIT PRIMITIVE note.
 *
 * CAUTION, API FAMILY MAY CHANGE AGAIN: that same DMP example's
 * freertos/app_proprietary.c defines a *live* (non-commented, must actually
 * compile) sl_rail_util_on_event(sl_rail_handle_t, sl_rail_events_t) --
 * lowercase family, contradicting this file's RAIL_* choice. Traced to real
 * cause, not guessed: rail_soc_railtest.slcp pulls in component id
 * `rail_util_init` (legacy, generates RAIL_* glue -- confirmed from its own
 * generated autogen/ output, see above); bt_rail_dmp_soc_empty_freertos.slcp
 * pulls in a DIFFERENTLY-NAMED component, `sl_rail_util_init` (confirmed by
 * grepping both .slcp files directly), which is what actually generates
 * sl_rail_* glue for that project shape. These are two distinct SDK
 * components that happen to produce similarly-named output files, not one
 * component behaving two ways. Net effect: this driver's RAIL_* calls are
 * still correct for the currently-generated rail_soc_railtest project this
 * file is checked against, but WILL LIKELY NEED REWRITING to sl_rail_* once
 * a real bt_rail_dmp_soc_empty_freertos project is generated for BRD2705A and
 * its own autogen/ output can be read -- same discipline as the original
 * RAIL_* vs sl_rail_* correction, not resolved by this comment alone. Until
 * that project exists on disk, do not further "fix" this guess in either
 * direction.
 */

#include <string.h>

#include "sl_subg_radio.h"
#include "rail.h"
#include "rail_types.h"
#include "sl_rail_util_init.h"   /* sl_rail_util_init(), sl_rail_util_get_handle() */

#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

static RAIL_Handle_t s_rail_handle;

/*
 * WAIT PRIMITIVE. Replaces the two busy-spin placeholders (RX-data-or-timeout,
 * TX-done) with a real FreeRTOS event group, now that FreeRTOS is confirmed
 * the sanctioned RTOS for this project shape (see the RTOS note above).
 *
 * Bits are set from sl_rail_util_on_event(), which the DMP example's own
 * readme.md states plainly runs "from interrupt context" -- so the ISR-safe
 * xEventGroupSetBitsFromISR() variant is required there, not the plain
 * xEventGroupSetBits() a task-context caller would use. sl_subg_abort() is
 * NOT confirmed to run from interrupt context (it is called from the BLE/APS
 * side -- see aps.c -- which the same readme says is NOT interrupt context),
 * so it uses the plain, non-ISR API.
 *
 * Grounded against the real headers this session:
 * ~/.silabs/slt/installs/conan/p/simpleca33d691c539/p/freertos/kernel/include/
 * event_groups.h (xEventGroupCreateStatic/WaitBits/SetBits/SetBitsFromISR, all
 * real declared signatures, not recalled) and
 * .../freertos/config/series2/FreeRTOSConfig.h (configTICK_RATE_HZ 1000,
 * configSUPPORT_STATIC_ALLOCATION 1 -- series2 is the right config file for
 * this Series-2 EFR32xG28 die). NOT yet checked against a real generated
 * project's own FreeRTOSConfig.h/portmacro.h pairing -- see
 * tools/check_compile.sh's freertos section for the caveat on what this
 * verifies and what it does not.
 */
#define SL_SUBG_EVT_RX_DATA (1UL << 0)
#define SL_SUBG_EVT_TX_DONE (1UL << 1)
#define SL_SUBG_EVT_ABORT   (1UL << 2)

/* Legacy TX_TIMEOUT (subg.c: SUBG_TX_DONE_WAIT_MS 150) -- a TX that never
 * completes must not hang the caller forever the way sl_subg_get_pkt()'s
 * 0-means-forever timeout legitimately can for RX.
 */
#define SL_SUBG_TX_DONE_TIMEOUT_MS 150U

static StaticEventGroup_t s_event_group_buf;
static EventGroupHandle_t s_event_group;

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
	BaseType_t higher_prio_task_woken = pdFALSE;
	bool rx_done = false;

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
				rx_done = true;
				break;
			}
			s_rx_buf[s_rx_count++] = byte;
		}

		/* A full buffer with no terminator seen is still a completed receive
		 * -- subg.c's equivalent loop breaks on EITHER condition
		 * (`count >= SUBG_MAX_PKT_LEN`), not terminator alone. Fixed here
		 * while adding the real wait primitive: the previous busy-spin
		 * version had the same gap (s_rx_have_data never set on this path),
		 * just harder to notice with nothing waiting on it.
		 */
		if (!rx_done && s_rx_count >= SL_SUBG_MAX_PKT_LEN) {
			s_rx_have_data = true;
			rx_done = true;
		}
	}

	if (rx_done) {
		(void)xEventGroupSetBitsFromISR(s_event_group, SL_SUBG_EVT_RX_DATA,
						&higher_prio_task_woken);
	}

	if (events & RAIL_EVENT_TX_PACKET_SENT) {
		s_tx_done = true;
		(void)xEventGroupSetBitsFromISR(s_event_group, SL_SUBG_EVT_TX_DONE,
						&higher_prio_task_woken);
	}

	/* TODO: RAIL_EVENT_RX_FIFO_OVERFLOW is a real, named event and is not
	 * handled here yet -- on the RFM69 side an equivalent overrun is a
	 * silent data-corruption risk that was specifically designed around
	 * (REG_IRQFLAGS2 FIFOOVERRUN handling in rf69_cfg_916[]). Needs the same
	 * care here before this is trusted against real traffic.
	 */

	portYIELD_FROM_ISR(higher_prio_task_woken);
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

	/* Statically allocated, matching the source's K_SEM_DEFINE-style static
	 * allocation rather than the heap. Must exist before RAIL_ConfigEvents()
	 * below unmasks events that could otherwise race this driver's own init.
	 */
	s_event_group = xEventGroupCreateStatic(&s_event_group_buf);
	if (s_event_group == NULL) {
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
		(void)xEventGroupClearBits(s_event_group, SL_SUBG_EVT_TX_DONE);

		RAIL_WriteTxFifo(s_rail_handle, data, len, true);
		RAIL_StartTx(s_rail_handle, s_channel, RAIL_TX_OPTIONS_DEFAULT, NULL);

		/* Bounded, not indefinite -- unlike sl_subg_get_pkt()'s
		 * caller-supplied listen window, a TX that never completes has no
		 * legitimate "wait forever" case. Mirrors subg.c's
		 * SUBG_TX_DONE_WAIT_MS. A timeout here leaves s_tx_done false; the
		 * loop proceeds to the next repeat regardless, matching the source's
		 * "count failures, do not abandon the burst" policy at the aps.c call
		 * site rather than this function inventing its own abort behaviour.
		 */
		(void)xEventGroupWaitBits(s_event_group, SL_SUBG_EVT_TX_DONE, pdTRUE,
					  pdFALSE, pdMS_TO_TICKS(SL_SUBG_TX_DONE_TIMEOUT_MS));

		if (repeat_interval_ms && i < repeat_cnt) {
			vTaskDelay(pdMS_TO_TICKS(repeat_interval_ms));
		}
	}
	s_tx_pkt_count++;
	return 0;
}

enum sl_subg_rx_status sl_subg_get_pkt(uint8_t *buf, uint8_t *len, uint32_t timeout_ms)
{
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
	(void)xEventGroupClearBits(s_event_group, SL_SUBG_EVT_RX_DATA);

	RAIL_StartRx(s_rail_handle, s_channel, NULL);

	/* timeout_ms == 0 means wait indefinitely, per sl_subg_get_pkt()'s
	 * contract (matches subg_get_pkt()'s own `timeout_ms > 0` guard) --
	 * portMAX_DELAY is FreeRTOS's real "block forever" value, not a very-long
	 * finite one, confirmed from task.h/projdefs.h this session.
	 */
	TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

	(void)xEventGroupWaitBits(s_event_group, SL_SUBG_EVT_RX_DATA | SL_SUBG_EVT_ABORT,
				  pdFALSE, pdFALSE, ticks);

	RAIL_Idle(s_rail_handle, RAIL_IDLE, true);

	/* s_abort_flag, not the event bit, is the source of truth here: the ABORT
	 * bit only guarantees xEventGroupWaitBits() woke up promptly, but
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

	s_last_rssi_dbm = RAIL_GetRssi(s_rail_handle, false);
	memcpy(buf, s_rx_buf, s_rx_count);
	*len = s_rx_count;
	s_rx_pkt_count++;
	return SL_SUBG_RX_OK;
}

void sl_subg_abort(void)
{
	s_abort_flag = true;
	/* Wakes a blocked sl_subg_get_pkt() immediately rather than leaving it to
	 * run out its full timeout -- the entire point of the source's
	 * Subg_SetIntFlg()/DIO1-adjacent design (see subg.c's own extensive note
	 * on why a level-vs-edge race made a bounded polling fallback necessary
	 * there; the FreeRTOS event group has no such race, so no fallback poll
	 * interval is needed here). NOT an ISR-context call -- see the WAIT
	 * PRIMITIVE note at the top of this file for why this uses the plain,
	 * non-FromISR API.
	 */
	(void)xEventGroupSetBits(s_event_group, SL_SUBG_EVT_ABORT);
}

void sl_subg_clear_abort(void)
{
	s_abort_flag = false;
	(void)xEventGroupClearBits(s_event_group, SL_SUBG_EVT_ABORT);
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
