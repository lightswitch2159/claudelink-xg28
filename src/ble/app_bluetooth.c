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

#include <stdio.h>
#include <string.h>

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

/*
 * Explicit advertising/scan-response data, not
 * sl_bt_legacy_advertiser_generate_data()'s automatic packing.
 *
 * AndroidAPS's BLE device scan filters on the advertised Insulin Pump
 * Service UUID -- confirmed against this project's own RFM69 reference
 * firmware (main.c), which puts that same 128-bit UUID in the primary
 * advertising packet for exactly that reason ("AAPS matches on it").
 * config/btconf/gatt_configuration.btconf marks that service
 * advertise="false", and nothing in the generated autogen/gatt_db.c
 * represents that flag in a form reachable from here, so rather than guess
 * how the closed-source stack's automatic packer treats it, the packet is
 * built explicitly to match the known-working reference byte-for-byte:
 * Flags + complete 128-bit Service UUID list in the advertising packet
 * (21 B), Complete Local Name in the scan response -- a 128-bit UUID and a
 * name don't both fit in the legacy 31-byte advertising packet.
 */
static void start_advertising(void)
{
	static const uint8_t adv_data[] = {
		0x02, 0x01, 0x06, /* Flags: LE General Discoverable, BR/EDR not supported */
		0x11, 0x07,       /* length 17, Complete List of 128-bit Service UUIDs */
		/* Insulin Pump Service 0235733b-99c5-4197-b856-69219c2a3845,
		 * little-endian on the wire -- matches gattdb_uuidtable_128_map's
		 * own encoding of the same UUID in autogen/gatt_db.c.
		 */
		0x45, 0x38, 0x2a, 0x9c, 0x21, 0x69, 0x56, 0xb8,
		0x97, 0x41, 0xc5, 0x99, 0x3b, 0x73, 0x35, 0x02,
	};
	uint8_t scan_rsp[2 + gattdb_device_name_len];
	uint8_t name[gattdb_device_name_len];
	size_t name_len = 0;
	sl_status_t sc;

	/* Read rather than hardcode: a Custom Name rename (see the
	 * gattdb_ips_custom_name case below) writes gattdb_device_name, and
	 * this function runs again on every reconnect (connection_closed_id),
	 * so the advertised name follows the rename from the next
	 * advertisement onward, matching this function's pre-existing
	 * contract.
	 */
	sc = sl_bt_gatt_server_read_attribute_value(gattdb_device_name, 0,
						    sizeof(name), &name_len, name);
	app_assert_status(sc);

	scan_rsp[0] = (uint8_t)(1 + name_len); /* AD type + name bytes */
	scan_rsp[1] = 0x09; /* Complete Local Name */
	memcpy(&scan_rsp[2], name, name_len);

	sc = sl_bt_legacy_advertiser_set_data(advertising_set_handle,
					      sl_bt_advertiser_advertising_data_packet,
					      sizeof(adv_data), adv_data);
	app_assert_status(sc);

	sc = sl_bt_legacy_advertiser_set_data(advertising_set_handle,
					      sl_bt_advertiser_scan_response_packet,
					      2 + name_len, scan_rsp);
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
		/* Permanent checkpoint, same reasoning as app.c's radio_rc
		 * print: the BLE connection lifecycle had zero visibility
		 * until now, and "AAPS shows problem connecting" needs this
		 * to tell apart a BLE-level failure from a failure further
		 * down (radio reaching the actual pump).
		 */
		printf("ble: connection opened (handle %u)\r\n", active_connection);
		break;

	/* The client disconnected. aps_set_active(false) aborts any
	 * in-flight receive (mirrors the legacy firmware's behaviour on a
	 * drop to advertising, per aps.c's own comment on this call) --
	 * matters because a CMD_SEND_AND_LISTEN can be mid-flight for up to
	 * the client's own timeout, and there is no longer anyone to answer.
	 */
	case sl_bt_evt_connection_closed_id:
		printf("ble: connection closed (handle %u, reason 0x%04x)\r\n",
		       evt->data.evt_connection_closed.connection,
		       evt->data.evt_connection_closed.reason);
		aps_set_active(false);
		active_connection = SL_BT_INVALID_CONNECTION_HANDLE;
		start_advertising();
		break;

	/* A remote GATT client wrote a local attribute. The IPS Data
	 * characteristic carries APS command frames; Custom Name renames the
	 * device. LED Mode is accepted by the GATT server automatically
	 * (Write property, no application handler required to ack it) and
	 * deliberately does nothing -- matches the legacy firmware's own
	 * empty LED Mode handler.
	 */
	case sl_bt_evt_gatt_server_attribute_value_id: {
		const sl_bt_evt_gatt_server_attribute_value_t *v =
			&evt->data.evt_gatt_server_attribute_value;

		if (v->attribute == gattdb_ips_data) {
			/*
			 * Confirmed against a real AndroidAPS log (RFSpy.
			 * writeToDataRaw, a CMD_SEND_AND_LISTEN frame: 14 05 00
			 * 00 00 00 00 00 00 04 E2 00 00 00 A7 56 07 93 8D 00 93,
			 * 21 B), cross-checked against RTT output from this
			 * firmware at the same moment: the Android BLE stack's
			 * writeCharacteristic() completed in one app-level call
			 * (21 B > the 20 B a single ATT write fits at the
			 * default 23 B MTU), but split it into a GATT long write
			 * -- a Prepare Write of the first 18 B followed by an
			 * Execute Write carrying the last 3 -- and delivered
			 * each to this handler as its own attribute_value_id
			 * event. The code this replaces ignored att_opcode and
			 * offset and dispatched v->value on every event, so
			 * aps_put_cmd() saw two independent, truncated
			 * fragments, both failed aps.c's self-consistency check
			 * ("malformed frame"), and AndroidAPS got no response to
			 * a real pump command -- "no response from RileyLink" in
			 * its own log, after 3 retries.
			 *
			 * gattdb_ips_data is automatic (non-"user") storage (see
			 * config/btconf/gatt_configuration.btconf), so the stack
			 * already reassembles queued Prepare Write chunks into
			 * the attribute's own backing buffer -- there is nothing
			 * to buffer here, only a dispatch to defer until the
			 * write is actually complete.
			 */
			int8_t rssi = 0;
			const uint8_t *cmd_data = v->value.data;
			uint16_t cmd_len = v->value.len;
			uint8_t full[gattdb_ips_data_len];

			if (v->att_opcode == sl_bt_gatt_prepare_write_request) {
				/* One chunk of a queued write, already stored
				 * by the stack at v->offset -- nothing is
				 * complete yet, wait for execute_write_request.
				 */
				break;
			}

			if (v->att_opcode == sl_bt_gatt_execute_write_request) {
				/* The queued write just committed. Read the
				 * attribute back rather than trust this event's
				 * own value/offset, which describe the commit
				 * operation, not the reassembled data.
				 */
				size_t full_len = 0;

				sc = sl_bt_gatt_server_read_attribute_value(
					gattdb_ips_data, 0, sizeof(full),
					&full_len, full);
				app_assert_status(sc);
				cmd_data = full;
				cmd_len = (uint16_t)full_len;
			}

			/* Best-effort -- aps_put_cmd()'s rssi parameter is
			 * recorded on the frame but not otherwise consumed
			 * by aps.c today (see its struct aps_req comment),
			 * so a failed RSSI read here degrades to 0 rather
			 * than blocking the command.
			 */
			(void)sl_bt_connection_get_median_rssi(v->connection, &rssi);

			aps_put_cmd(cmd_data, cmd_len, rssi);
		} else if (v->attribute == gattdb_ips_custom_name) {
			/*
			 * Legacy behaviour (RFM69 reference repo's ips.h:
			 * IPS_EVT_CUS_NAME_RX -> "persist + disconnect") --
			 * reproduced here as far as this project's current
			 * infrastructure allows: no flash-backed settings
			 * storage exists yet, so this changes the advertised
			 * name for the remainder of this power cycle only,
			 * not across a reboot. "Persist" in the fullest sense
			 * needs NVM3 (or similar) wired up first; not done.
			 *
			 * The Bluetooth stack's own "full local name" in
			 * advertising data (see sl_bt_legacy_advertiser_
			 * generate_data()'s doc comment) is sourced from the
			 * standard Device Name characteristic (0x2A00), not
			 * from Custom Name directly -- there is no separate
			 * "set advertised name" command in this SDK, this is
			 * the sanctioned mechanism. Device Name is declared
			 * FIXED length (gattdb_device_name_len, currently 12
			 * -- see config/btconf/gatt_configuration.btconf),
			 * narrower than Custom Name's own 30-byte capacity,
			 * so a longer name is truncated here rather than
			 * risking a rejected write against a fixed-length
			 * attribute of the wrong size.
			 *
			 * Deliberately does NOT call start_advertising() here:
			 * this connection is about to close, and the
			 * connection_closed_id case above already regenerates
			 * advertising data (re-reading the just-updated Device
			 * Name) and restarts advertising when that happens.
			 * Calling it here too would try to start an
			 * already-active advertising set a second time.
			 */
			uint16_t name_len = v->value.len;
			sl_status_t name_sc;

			if (name_len > gattdb_device_name_len) {
				name_len = gattdb_device_name_len;
			}

			name_sc = sl_bt_gatt_server_write_attribute_value(
				gattdb_device_name, 0, name_len, v->value.data);
			if (name_sc == SL_STATUS_OK &&
			    v->connection != SL_BT_INVALID_CONNECTION_HANDLE) {
				(void)sl_bt_connection_close(v->connection);
			}
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
