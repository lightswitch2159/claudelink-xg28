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

#include "aps.h"
#include "drivers/rail/sl_subg_radio.h"

void app_init(void)
{
	int radio_rc;

	/* Order matters only in that aps_init() does not itself call
	 * sl_subg_radio_init() -- see aps.c's own comment on that -- so it
	 * must happen here, once, before anything can reach the radio
	 * driver's other entry points. Neither function blocks or depends on
	 * BLE having booted yet; sl_bt_on_event()'s boot case runs
	 * independently.
	 */
	radio_rc = sl_subg_radio_init();
	/* Bare printf() over the RTT console -- src/aps/aps.c's own APS_LOG_*
	 * macros are still no-ops (see item 5 in its file banner), so this is
	 * the only boot-path visibility that exists right now. Worth keeping
	 * permanently: a failed radio init here would otherwise be as silent as
	 * the real bug this same printf caught on first hardware bring-up (see
	 * git history) -- sl_subg_radio_init() returning nonzero deserves to be
	 * visible, not just checked and dropped.
	 */
	printf("app_init: sl_subg_radio_init() = %d\r\n", radio_rc);

	aps_init();
}
