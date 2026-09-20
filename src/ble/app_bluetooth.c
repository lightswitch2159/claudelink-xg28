/*
 * Core Bluetooth application logic -- APS/BLE bridge.
 *
 * The boot case of sl_bt_on_event() below (advertising set creation, timing,
 * start) is substantially unchanged from the generated skeleton of this file
 * from Silicon Labs' "Bluetooth RAIL DMP - SoC Empty Micrium OS" example, so
 * that file's own notice is kept rather than just cited:
 *
 *   Copyright 2020 Silicon Laboratories Inc. www.silabs.com
 *   SPDX-License-Identifier: Zlib
 *   This software is provided 'as-is', without any express or implied
 *   warranty. In no event will the authors be held liable for any damages
 *   arising from the use of this software. Permission is granted to anyone
 *   to use this software for any purpose, including commercial
 *   applications, and to alter it and redistribute it freely, subject to
 *   the following restrictions: (1) the origin of this software must not be
 *   misrepresented; you must not claim that you wrote the original
 *   software -- an acknowledgment in the product documentation would be
 *   appreciated but is not required; (2) altered source versions must be
 *   plainly marked as such, and must not be misrepresented as being the
 *   original software; (3) this notice may not be removed or altered from
 *   any source distribution.
 *
 * Everything from sl_bt_evt_gatt_server_attribute_value_id onward, and the
 * connection-lifecycle handling, is new -- SPDX-License-Identifier for that
 * part: GPL-2.0-only, consistent with the rest of this repository. The
 * connection_opened/connection_closed cases add real behaviour (tracking
 * active_connection, aps_set_active()) on top of the original's bare
 * advertising-restart logic.
 *
 * This file is the entire BLE side of the RileyLink emulation: it dispatches
 * GATT writes on the IPS Data characteristic into aps_put_cmd(), and provides
 * the strong override of aps_transport_send() (weak default in
 * aps_transport.c) that turns an APS response into the real
 * write-then-notify handshake AndroidAPS/Loop expect. See
 * config/btconf/gatt_configuration.btconf for the GATT layout this drives
 * (UUIDs, properties, lengths) -- reproduced exactly from the legacy
 * RileyLink firmware, gattdb_ips_* symbol names below come from the real
 * generated autogen/gatt_db.h for this project, not guessed.
 *
 * INTEGRATION STATUS: this file, and the rest of src/, are checked
 * standalone against this project's real toolchain/include/define set
 * (tools/check_compile.sh) but are NOT YET wired into
 * orangelink_xg28.slcp's own source list -- a real Simplicity Studio build
 * of that project does not yet compile the files under src/aps, under
 * src/drivers/rail, or this one. That's the concrete remaining step before
 * this becomes a flashable image; not done here because it means editing
 * generated project
 * metadata (the .slcp source/include lists) outside Studio's own tooling,
 * which this session has already been burned by once (see the Range Test
 * component-entanglement note in git history) -- safer to do through
 * Studio's Project Configurator directly, with someone watching for
 * validation errors, than to hand-edit it blind.
 */

#include "app_assert.h"
#include "sl_bluetooth.h"
#include "app_bluetooth.h"
#include "gatt_db.h"

#include "aps.h"
#include "aps_transport.h"

/* The advertising set handle allocated from the Bluetooth stack. */
static uint8_t advertising_set_handle = 0xff;

/*
 * The one connection this bridge talks to. RileyLink emulation is inherently
 * single-client (one phone at a time) -- the legacy protocol has no
 * multi-connection story, and aps_transport_send() below has no connection
 * parameter to disambiguate against, so there is nowhere to route a second
 * client's responses even if the stack allowed a second connection.
 * SL_BT_INVALID_CONNECTION_HANDLE (0xff, confirmed from the real sl_bt_api.h
 * this session) is both the "no connection" sentinel and coincidentally the
 * same reset value the generated skeleton already used for
 * advertising_set_handle.
 */
static uint8_t active_connection = SL_BT_INVALID_CONNECTION_HANDLE;

/*
 * Response Count: uint8_t, incremented per successful response, wrapping
 * 0x00-0xFE (never taking the value 0xFF) -- legacy semantics, reproduced
 * exactly. See docs/gatt-service-spec.md section 3 in the RFM69 reference
 * repo for the full handshake description this implements.
 */
static uint8_t response_count;

static void start_advertising(void)
{
	sl_status_t sc;

	sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
						   sl_bt_advertiser_general_discoverable);
	app_assert_status(sc);

	sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
					   sl_bt_legacy_advertiser_connectable);
	app_assert_status(sc);
}

/* CMD_GET_STATE et al still return a real answer to the host even when the
 * radio side isn't up yet -- aps_put_cmd()/aps_set_active() don't depend on
 * sl_subg_radio_init() having succeeded, matching the source's own layering
 * (BLE, radio, and protocol dispatch are independently initialized). Both
 * are called from app_init(), not from here -- this file owns the BLE event
 * loop, not application bring-up order.
 */

void sl_bt_on_event(sl_bt_msg_t *evt)
{
	sl_status_t sc;

	switch (SL_BT_MSG_ID(evt->header)) {
	/* This event indicates the device has started and the radio is
	 * ready. Do not call any stack command before receiving this boot
	 * event!
	 */
	case sl_bt_evt_system_boot_id:
		sc = sl_bt_advertiser_create_set(&advertising_set_handle);
		app_assert_status(sc);

		/* 100 ms advertising interval -- unchanged from the generated
		 * skeleton; not a RileyLink-protocol requirement, just a
		 * reasonable discoverability/power tradeoff, revisit if
		 * connection time on the bench turns out to matter.
		 */
		sc = sl_bt_advertiser_set_timing(advertising_set_handle,
						 160, 160, 0, 0);
		app_assert_status(sc);

		start_advertising();
		break;

	/* A client connected. Track the connection handle for
	 * aps_transport_send()'s notifications, and let the protocol layer
	 * start accepting commands.
	 */
	case sl_bt_evt_connection_opened_id:
		active_connection = evt->data.evt_connection_opened.connection;
		response_count = 0;
		aps_set_active(true);
		break;

	/* The client disconnected. aps_set_active(false) aborts any
	 * in-flight receive (mirrors the legacy firmware's behaviour on a
	 * drop to advertising, per aps.c's own comment on this call) --
	 * matters because a CMD_SEND_AND_LISTEN can be mid-flight for up to
	 * the client's own timeout, and there is no longer anyone to answer.
	 */
	case sl_bt_evt_connection_closed_id:
		aps_set_active(false);
		active_connection = SL_BT_INVALID_CONNECTION_HANDLE;
		start_advertising();
		break;

	/* A remote GATT client wrote a local attribute -- the only write we
	 * act on is the IPS Data characteristic, which carries APS command
	 * frames. Writes to Custom Name/LED Mode are accepted by the GATT
	 * server automatically (Write property, no application handler
	 * required to ack them) but have no behaviour wired up here yet --
	 * matches the legacy LED Mode handler being empty, but Custom Name
	 * SHOULD eventually persist and change the advertised name; not done.
	 */
	case sl_bt_evt_gatt_server_attribute_value_id: {
		const sl_bt_evt_gatt_server_attribute_value_t *v =
			&evt->data.evt_gatt_server_attribute_value;

		if (v->attribute == gattdb_ips_data) {
			int8_t rssi = 0;

			/* Best-effort -- aps_put_cmd()'s rssi parameter is
			 * recorded on the frame but not otherwise consumed
			 * by aps.c today (see its struct aps_req comment),
			 * so a failed RSSI read here degrades to 0 rather
			 * than blocking the command.
			 */
			(void)sl_bt_connection_get_median_rssi(v->connection, &rssi);

			aps_put_cmd(v->value.data, v->value.len, rssi);
		}
		break;
	}

	default:
		break;
	}
}

/*
 * aps_transport_send() -- strong override of the weak no-op default in
 * aps_transport.c (src/aps/). Implements the legacy handshake exactly, per
 * docs/gatt-service-spec.md section 3 in the RFM69 reference repo:
 *
 *   1. store @p data as the Data characteristic's value
 *   2. increment Response Count and notify it
 *
 * Ordering is mandatory, not stylistic: the client only reads Data after
 * seeing the Response Count notification, so the value has to be in place
 * first. The counter is only advanced when step 1 actually succeeds --
 * mirrors the legacy firmware's own
 * `if (ble_ips_data_send(...) == NRF_SUCCESS) { ble_ips_response_cnt_notify(...); }`
 * gate, reproduced here as the sl_status_t check below.
 */
void aps_transport_send(const uint8_t *data, uint16_t len)
{
	sl_status_t sc;

	if (active_connection == SL_BT_INVALID_CONNECTION_HANDLE) {
		/* No client to answer -- e.g. a response computed after the
		 * link already dropped. Legacy IPS_EVT_UNDELIVERABLE case;
		 * this port just drops the response rather than tearing the
		 * (already gone) connection down again.
		 */
		return;
	}

	sc = sl_bt_gatt_server_write_attribute_value(gattdb_ips_data, 0, len, data);
	if (sc != SL_STATUS_OK) {
		return;
	}

	/* Wrap 0x00-0xFE, never 0xFF -- legacy semantics (see the field
	 * comment above), checked BEFORE incrementing so the sequence really
	 * is 0x00, 0x01, ..., 0xFE, 0x00, ... rather than skipping 0x00 on
	 * the wrap.
	 */
	if (response_count >= 0xFE) {
		response_count = 0;
	} else {
		response_count++;
	}

	sc = sl_bt_gatt_server_write_attribute_value(gattdb_ips_response_count, 0,
						     sizeof(response_count),
						     &response_count);
	if (sc != SL_STATUS_OK) {
		return;
	}

	/* Notification, not indication -- matches the GATT properties
	 * declared for Response Count (Read, Notify only) in
	 * config/btconf/gatt_configuration.btconf. Silently drops if the
	 * client hasn't subscribed (CCCD not enabled) or the link can't
	 * accept it right now (e.g. an ATT transaction already in flight) --
	 * SL_STATUS_OK is not guaranteed here and this deliberately does not
	 * retry; a client that isn't listening for Response Count will just
	 * time out its own command, same externally-visible behaviour as the
	 * legacy firmware's un-checked notify call.
	 */
	(void)sl_bt_gatt_server_send_notification(active_connection,
						  gattdb_ips_response_count,
						  sizeof(response_count),
						  &response_count);
}
