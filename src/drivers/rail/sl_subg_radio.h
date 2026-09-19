/*
 * Sub-GHz radio driver -- EFR32xG28 native RAIL, 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Not a port of src/drivers/rf69/rf69.h. That header is a register-primitive
 * interface (read_reg/write_reg/set_mode/fifo_write_byte...) because the
 * RFM69 is an external SPI peripheral with addressable registers. RAIL has no
 * registers to poke -- it is a packet-oriented state machine reached through
 * function calls and an event callback. So this header exposes the
 * OPERATIONS subg.c actually needs (init, send, receive-with-timeout, RSSI,
 * power), matching rf69.h's role in the system rather than its shape.
 *
 * API FAMILY, CORRECTED ONCE ALREADY: this uses RAIL_* (PascalCase), not the
 * newer sl_rail_* family. The header comments in this SDK mark RAIL_Init /
 * RAIL_StartTx / RAIL_StartRx / RAIL_ConfigChannels @deprecated, and an
 * earlier version of this file was written against sl_rail_* on that basis.
 * That was wrong: Simplicity Studio's own Radio Configurator generates
 * RAIL_ChannelConfig_t (not sl_rail_channel_config_t) for this exact project,
 * and its own generated init glue
 * (SimplicityStudio/v6_workspace/rail_soc_railtest/autogen/sl_rail_util_init.c)
 * calls RAIL_Init/RAIL_ConfigData/RAIL_ConfigChannels throughout -- not one
 * sl_rail_* call anywhere in it. The deprecation tags are real, but this
 * project template is built on the "deprecated" API regardless, and passing
 * the generated channel config to sl_rail_config_channels() would be a type
 * mismatch (sl_rail_channel_config_t != RAIL_ChannelConfig_t) even if it
 * compiled. Verified by reading the actual generated files, not asserted.
 *
 * Bring-up should call the generated sl_rail_util_init() (declared in
 * autogen/sl_rail_util_init.h) rather than reimplement RAIL_Init +
 * RAIL_ConfigChannels by hand -- it is Silicon Labs' own tested glue, already
 * wired to the real MDT_OOK channel config (autogen/rail_config.c), and
 * retrieving the resulting handle via sl_rail_util_get_handle() avoids
 * duplicating what it already does correctly.
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
 * Cross-checked again against the generated rail_config.c: baseFrequency =
 * 916000000, channelSpacing = 548000 -- channel 1 lands on 916,548,000 Hz,
 * channelNumberEnd = 20 so channel 1 is in range.
 */
#define SL_SUBG_CHANNEL 1U
#define SL_SUBG_FREQ_HZ 916548000U

/* Channel map, read from the generated autogen/rail_config.c this session:
 * baseFrequency = 916000000, channelSpacing = 548000, channels 0..20. The APS
 * protocol layer (src/aps/aps.c, ported from the legacy RileyLink command set)
 * asks for an arbitrary frequency in Hz, computed from three CC111x-style
 * register bytes the host sends -- there is no RAIL call that tunes to an
 * arbitrary Hz value directly, only RAIL_StartTx/RAIL_StartRx(channel). So
 * sl_subg_set_freq() maps the requested Hz onto the nearest in-range channel
 * of this same static config, rather than reconfiguring the PHY.
 */
#define SL_SUBG_BASE_FREQ_HZ 916000000U
#define SL_SUBG_CHANNEL_SPACING_HZ 548000U
#define SL_SUBG_CHANNEL_MIN 0U
#define SL_SUBG_CHANNEL_MAX 20U

/* Matches SUBG_MAX_PKT_LEN in the working firmware (src/subg/subg.h) --
 * deliberately kept identical so the protocol layer above this driver can be
 * ported with the same buffer sizing, not a new, independently-chosen limit.
 * Also the Radio Configurator's Frame Fixed Length payload size (config saved
 * in Studio: FIXED_LENGTH, 107 bytes), so the radio's own declared frame
 * length matches what the application considers the real ceiling.
 */
#define SL_SUBG_MAX_PKT_LEN 107U

enum sl_subg_rx_status {
	SL_SUBG_RX_OK = 0,
	SL_SUBG_RX_TIMEOUT,
	SL_SUBG_RX_INTERRUPTED,
};

/**
 * @brief Bring RAIL up via the generated sl_rail_util_init(), bind TX/RX
 * FIFOs, configure the events this driver needs.
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
 * the radio is configured FIXED_LENGTH at the maximum (107 bytes), not
 * because frames are really fixed-length, but because RAIL -- like the RFM69
 * -- has no "terminate on a sentinel byte" packet mode. subg_get_pkt() on the
 * RFM69 declares a generous upper bound and drains the FIFO incrementally via
 * the DIO1/FifoNotEmpty interrupt, watching for the 0x00 terminator itself
 * and stopping long before the radio's own length counter would complete.
 * The RAIL equivalent of that interrupt is RAIL_EVENT_RX_FIFO_ALMOST_FULL
 * (confirmed present in rail_types.h this session), delivered through the
 * RAIL_Config_t::eventsCallback set up by the generated init code -- so this
 * function is implemented the same way: start_rx(), drain on that event via
 * RAIL_ReadRxFifo(), terminate on 0x00, RAIL_Idle() when done.
 */
enum sl_subg_rx_status sl_subg_get_pkt(uint8_t *buf, uint8_t *len,
				       uint32_t timeout_ms);

/** @brief Interrupt an in-progress sl_subg_get_pkt() from another context. */
void sl_subg_abort(void);
void sl_subg_clear_abort(void);

/**
 * @brief Last measured reply RSSI, dBm. Maps directly onto RAIL_GetRssi() --
 * unlike the RFM69 port, no first-byte-latch workaround should be needed,
 * since RAIL reports genuine per-packet RSSI rather than a live, freely
 * drifting register (see MIGRATION_NOTES.md section 21 on why that workaround
 * existed at all on the RFM69).
 */
int16_t sl_subg_get_last_rssi(void);

/**
 * @brief Set transmit power, in whole dBm.
 *
 * Grounded this session against the real rail.h: RAIL_SetTxPowerDbm(handle,
 * RAIL_TxPower_t power) takes DECI-dBm (rail_types.h: typedef int16_t
 * RAIL_TxPower_t; see the RAIL_SetTxPowerDbm doc comment's own example, "100
 * deci-dBm, 10 dBm"). This function takes whole dBm and multiplies by 10 --
 * unlike the RFM69 driver's rf69_set_power_level(), which takes a raw PA
 * register value (0-31), so callers porting from subg.c cannot reuse the old
 * constants (SUBG_PA_LEVEL_MIN/DEFAULT) unchanged; see aps.c/subg equivalent
 * for the new call sites. Requires RAIL_ConfigTxPower() to have already run,
 * which the generated sl_rail_util_init() is expected to do -- not
 * independently confirmed against this exact project's autogen output.
 */
int sl_subg_set_power_level(int16_t dbm);

/** @brief RX/TX packet counters, for CMD_GET_STATISTICS. Mirror
 * subg_get_rx_count()/subg_get_tx_count() in the RFM69 port.
 */
uint16_t sl_subg_get_rx_count(void);
uint16_t sl_subg_get_tx_count(void);

/**
 * @brief Tune to the channel nearest @p hz within the static channel map
 * (SL_SUBG_BASE_FREQ_HZ + n * SL_SUBG_CHANNEL_SPACING_HZ, n in
 * [SL_SUBG_CHANNEL_MIN, SL_SUBG_CHANNEL_MAX]). Takes effect on the next
 * sl_subg_send_pkt()/sl_subg_get_pkt() call.
 *
 * @return 0 if @p hz is within the map (channel selected), -1 if it falls
 * outside the nearest-channel's spacing/2 tolerance and was ignored -- mirrors
 * rf69_set_freq() returning success/failure to aps.c's apply_pending_freq(),
 * which already logs and discards out-of-band requests using that return.
 */
int sl_subg_set_freq(uint32_t hz);

/** @brief Hz of the channel sl_subg_set_freq() most recently selected
 * (or SL_SUBG_FREQ_HZ / channel SL_SUBG_CHANNEL if never called).
 */
uint32_t sl_subg_get_freq(void);

/** @brief Reset to the default channel (SL_SUBG_CHANNEL). Mirrors
 * rf69_config_916() as used by aps.c's CMD_RESET_RADIO_CFG -- RAIL's PHY
 * config is static (set once by sl_rail_util_init()), so there is no register
 * table to reload; only the channel selection is reset.
 */
void sl_subg_reset_radio_cfg(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_SL_SUBG_RADIO_H_ */
