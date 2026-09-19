/*
 * Sub-GHz radio driver -- EFR32xG28 native RAIL, 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Not a port of src/drivers/rf69/rf69.h. That header is a register-primitive
 * interface (read_reg/write_reg/set_mode/fifo_write_byte...) because the
 * RFM69 is an external SPI peripheral with addressable registers. RAIL has no
 * registers to poke -- it is a packet-oriented state machine reached through
 * function calls (sl_rail_start_tx(), sl_rail_start_rx(), ...) and an event
 * callback, not a byte-level bus. So this header exposes the OPERATIONS
 * subg.c actually needs (init, send, receive-with-timeout, RSSI, power),
 * matching rf69.h's role in the system rather than its shape.
 *
 * Every sl_rail_*() signature referenced in the .c file was checked against
 * the real headers in this workspace
 * (modules/hal/silabs/simplicity_sdk/platform/radio/rail_lib/common/sl_rail.h,
 * fetched this session) rather than written from memory. Anywhere that could
 * not be grounded that way -- chiefly the exact generated PHY config symbol
 * name Simplicity Studio's Radio Configurator will emit for the MDT_OOK
 * protocol, and the TX power control API -- is marked TODO below rather than
 * guessed. Fill those in from the actual generated project once exported.
 *
 * IMPORTANT: RAIL 2.x (RAIL_StartTx, RAIL_Init, RAIL_ConfigChannels, ...) is
 * deprecated in this SDK generation ("RAIL 3"), in favour of the sl_rail_*()
 * names used throughout this file. Do not reintroduce the PascalCase RAIL_*
 * calls -- they exist in the headers only as deprecated wrappers.
 */

#ifndef ORANGELINK_SL_SUBG_RADIO_H_
#define ORANGELINK_SL_SUBG_RADIO_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 916.548 MHz, verified this session to 1 Hz against the working RFM69
 * config's own FRF register (0xE52312) and independently against the
 * nRF52840 branch's self-test measuring 916,547,973 Hz on real hardware.
 * In the Radio Configurator this is channel 1 of MDT_OOK-Channel_Group_1
 * (base 916 MHz, 548 kHz spacing).
 */
#define SL_SUBG_CHANNEL 1U
#define SL_SUBG_FREQ_HZ 916548000U

/* Matches SUBG_MAX_PKT_LEN in the working firmware (src/subg/subg.h) --
 * deliberately kept identical so the protocol layer above this driver can be
 * ported with the same buffer sizing, not a new, independently-chosen limit.
 * Also the Radio Configurator's Frame Fixed Length payload size, so the
 * radio's own declared frame length matches what the application considers
 * the real ceiling.
 */
#define SL_SUBG_MAX_PKT_LEN 107U

enum sl_subg_rx_status {
	SL_SUBG_RX_OK = 0,
	SL_SUBG_RX_TIMEOUT,
	SL_SUBG_RX_INTERRUPTED,
};

/**
 * @brief Bring RAIL up on this chip: sl_rail_init(), FIFO buffers bound,
 * channel config applied, radio left idle.
 *
 * TODO: takes no PHY config argument yet because the generated
 * MDT_OOK-Channel_Group_1 channel config symbol (the sl_rail_channel_config_t
 * this project's Radio Configurator emits) is not available until the real
 * project is exported. Once it is, this almost certainly needs to accept or
 * reference that generated table, the same way sl_rail_config_channels()
 * takes a `const sl_rail_channel_config_t *` in the real header.
 */
int sl_subg_radio_init(void);

/**
 * @brief Send @p len bytes, optionally repeated @p repeat_cnt additional
 * times with @p repeat_interval_ms between repeats (0 = back-to-back).
 *
 * Mirrors subg_send_pkt()'s existing contract exactly -- same parameters,
 * same "0 interval means stream repeats with no gap" behaviour used for the
 * 201-frame pump wake burst -- so subg.c's call sites do not need to change
 * shape, only which radio driver they link against.
 */
int sl_subg_send_pkt(const uint8_t *data, uint8_t len, uint8_t repeat_cnt,
		     uint16_t repeat_interval_ms);

/**
 * @brief Receive into @p buf (capacity SL_SUBG_MAX_PKT_LEN), for up to
 * @p timeout_ms. Returns the byte count actually received via *len.
 *
 * Framing note, carried over from the RFM69 driver rather than reinvented:
 * the radio is configured FIXED_LENGTH at the maximum (107 bytes), not because
 * frames are really fixed-length, but because RAIL -- like the RFM69 -- has
 * no "terminate on a sentinel byte" packet mode. subg_get_pkt() on the RFM69
 * declares a generous upper bound and drains the FIFO incrementally via the
 * DIO1/FifoNotEmpty interrupt, watching for the 0x00 terminator itself and
 * stopping long before the radio's own length counter would complete. The
 * RAIL equivalent of that interrupt is SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL
 * (confirmed present in sl_rail_types.h this session) delivered through the
 * events_callback configured at sl_rail_init() time -- so this function
 * should be implemented the same way: start_rx(), drain on that event via
 * sl_rail_read_rx_fifo(), terminate on 0x00, sl_rail_idle() when done. Not yet
 * implemented pending the real project export.
 */
enum sl_subg_rx_status sl_subg_get_pkt(uint8_t *buf, uint8_t *len,
				       uint32_t timeout_ms);

/** @brief Interrupt an in-progress sl_subg_get_pkt() from another context. */
void sl_subg_abort(void);
void sl_subg_clear_abort(void);

/**
 * @brief Last measured reply RSSI, dBm. Maps directly onto sl_rail_get_rssi()
 * -- unlike the RFM69 port, no first-byte-latch workaround should be needed,
 * since RAIL reports genuine per-packet RSSI rather than a live, freely
 * drifting register (see MIGRATION_NOTES.md section 21 on why that workaround
 * existed at all on the RFM69).
 */
int16_t sl_subg_get_last_rssi(void);

/**
 * @brief Set transmit power. TODO: verified only that this concept has a real
 * API in RAIL (sl_rail_config_tx_power() or similar was referenced in the
 * headers but not pinned down this session) -- do not assume a specific
 * function name here without checking sl_rail.h again.
 */
int sl_subg_set_power_level(int16_t dbm);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_SL_SUBG_RADIO_H_ */
