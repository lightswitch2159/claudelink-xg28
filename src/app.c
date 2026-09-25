/*
 * Application entry point.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * app_init() previously called app_proprietary_init(), which created its
 * own Micrium OS task and defined its own (empty, stub) sl_rail_util_on_event()
 * -- both now redundant and, for the callback, actively conflicting:
 * sl_subg_radio.c defines the real sl_rail_util_on_event(), and aps.c
 * creates its own dedicated dispatch task via aps_init(). See the
 * INTEGRATION STATUS note in src/ble/app_bluetooth.c for what's still needed
 * before this actually builds through Simplicity Studio.
 */

#include <stdio.h>

#include "sl_main_init.h"
#include "sl_power_manager.h"

#include "aps.h"
#include "debug/dbg_log.h"
#include "drivers/rail/sl_subg_radio.h"

/*
 * DEBUG AID, not a permanent fix -- remove before any real battery-powered
 * use. This project has SL_CATALOG_POWER_MANAGER_DEEPSLEEP_PRESENT enabled
 * and nothing in this codebase ever calls sl_power_manager_add_em_requirement(),
 * so the RTOS idle task is free to drop the whole system into EM2/EM3
 * whenever nothing else is pending. TESTED, RESULT NEGATIVE: this was added
 * to check whether that deep sleep was the cause of `commander rtt connect`
 * dying with "ERROR: Could not write data to target" a few seconds into
 * every session -- with EM1 forced (blocking EM2/EM3/EM4 entirely, so the
 * core never actually stops executing) RTT still died the same way, ruling
 * out deep sleep as the mechanism. (Also ruled out: blocking printf -- this
 * project's IOSTREAM_RTT_UP_MODE is SEGGER_RTT_MODE_NO_BLOCK_TRIM, so printf
 * can never stall target execution regardless of host drain rate.) Left
 * enabled since it's otherwise harmless and rules out one more variable; see
 * dbg_log.h for the actual fix -- a local RAM log this doesn't depend on a
 * live debug connection surviving the whole test run.
 */
#define DEBUG_FORCE_EM1_FOR_RTT 1

void app_init(void)
{
	int radio_rc;

	dbg_log_init();

#if DEBUG_FORCE_EM1_FOR_RTT
	sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
#endif

	/* Order matters only in that aps_init() does not itself call
	 * sl_subg_radio_init() -- see aps.c's own comment on that -- so it
	 * must happen here, once, before anything can reach the radio
	 * driver's other entry points. Neither function blocks or depends on
	 * BLE having booted yet; sl_bt_on_event()'s boot case runs
	 * independently.
	 */
	radio_rc = sl_subg_radio_init();
	/* dbg_printf(), not bare printf() -- see dbg_log.h. src/aps/aps.c's own
	 * APS_LOG_* macros are still no-ops (see item 5 in its file banner), so
	 * this is the only boot-path visibility that exists right now. Worth
	 * keeping permanently: a failed radio init here would otherwise be as
	 * silent as the real bug this same printf caught on first hardware
	 * bring-up (see git history) -- sl_subg_radio_init() returning nonzero
	 * deserves to be visible, not just checked and dropped.
	 */
	dbg_printf("app_init: sl_subg_radio_init() = %d\r\n", radio_rc);

	aps_init();
}
