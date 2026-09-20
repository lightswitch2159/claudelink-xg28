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

#include "sl_main_init.h"

#include "aps.h"
#include "drivers/rail/sl_subg_radio.h"

void app_init(void)
{
	/* Order matters only in that aps_init() does not itself call
	 * sl_subg_radio_init() -- see aps.c's own comment on that -- so it
	 * must happen here, once, before anything can reach the radio
	 * driver's other entry points. Neither function blocks or depends on
	 * BLE having booted yet; sl_bt_on_event()'s boot case runs
	 * independently.
	 */
	(void)sl_subg_radio_init();
	aps_init();
}
