/*
 * APS response transport -- decouples aps.c from whatever carries responses
 * back to the host (BLE GATT notification on the RFM69 port's ble/ips.h, not
 * yet ported here). SPDX-License-Identifier: GPL-2.0-only
 *
 * The RFM69 port's aps.c called ips_send_response() directly, a BLE-specific
 * function. This project has no BLE/DMP layer yet (see README.md status), so
 * aps.c is ported against this narrow interface instead, and the BLE layer
 * implements aps_transport_send() once it exists -- exactly the same
 * weak-symbol-override shape already used for sl_rail_util_on_event() in
 * src/drivers/rail/sl_subg_radio.c, applied one layer up.
 *
 * A weak default is provided in aps_transport.c so aps.c and its tests/compile
 * checks link and run standalone before the BLE layer exists.
 */

#ifndef ORANGELINK_APS_TRANSPORT_H_
#define ORANGELINK_APS_TRANSPORT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Send @p len bytes of an APS response frame to the host. */
void aps_transport_send(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_APS_TRANSPORT_H_ */
