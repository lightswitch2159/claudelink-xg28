/*
 * Weak default for aps_transport_send() -- see aps_transport.h.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Overridden by strong linkage from the BLE/DMP layer once it exists, the same
 * pattern used for sl_rail_util_on_event() in sl_subg_radio.c. Until then this
 * silently drops responses, which is correct for compiling and bench-testing
 * aps.c's command parsing without a radio link or a BLE stack.
 */

#include "aps_transport.h"

__attribute__((weak)) void aps_transport_send(const uint8_t *data, uint16_t len)
{
	(void)data;
	(void)len;
}
