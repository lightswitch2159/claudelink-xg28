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
 * API FAMILY, CORRECTED A SECOND TIME -- this is now sl_rail_* (lowercase),
 * not RAIL_*. Two real, conflicting pieces of generated evidence exist in
 * this repo's history, and the discriminator turned out to be WHICH SDK
 * component a project pulls in, not a global SDK-wide answer:
 *
 *   - rail_soc_railtest (RAIL - SoC RAILtest example) pulls in component id
 *     `rail_util_init` and its generated autogen/sl_rail_util_init.c calls
 *     RAIL_Init/RAIL_ConfigChannels throughout -- confirmed by reading that
 *     file directly, which is what the driver was first (correctly, for that
 *     project) written against.
 *   - rail_bt_dmp_soc_range_test (RAIL Bluetooth DMP - SoC Range Test
 *     example, generated for BRD2705A this session) pulls in a
 *     DIFFERENTLY-NAMED component, `sl_rail_util_init`, whose generated
 *     autogen/sl_rail_util_callbacks.c defines a REAL (not commented-out)
 *     sl_rail_util_on_event(sl_rail_handle_t, sl_rail_events_t) as the weak
 *     stub this driver's callback overrides, and autogen/sl_rail_util_init.h
 *     declares sl_rail_util_get_handle() returning sl_rail_handle_t. This is
 *     the DMP-shaped project this driver actually needs to integrate with, so
 *     this rewrite follows it.
 *
 * Every sl_rail_*() signature below (set_tx_fifo/set_rx_fifo/write_tx_fifo/
 * read_rx_fifo/start_tx/start_rx/idle/get_rssi/set_tx_power_dbm/
 * config_events) was checked against the real header in this exact project's
 * own copied SDK
 * (SimplicityStudio/v6_workspace/rail_bt_dmp_soc_range_test/simplicity_sdk_2026.6.1/rail_library/common/sl_rail.h),
 * not recalled from memory or extrapolated from the RAIL_* names. Some
 * signatures genuinely differ beyond casing -- notably sl_rail_get_rssi()
 * takes a microsecond wait_timeout, not the RAIL_GetRssi() bool wait flag it
 * replaces, and sl_rail_set_tx_fifo()/sl_rail_set_rx_fifo() take a
 * sl_rail_fifo_buffer_align_t* (a plain uint32_t alias, per sl_rail_types.h,
 * requiring the FIFO buffers below to be word-array-typed) rather than a bare
 * uint8_t*.
 *
 * RTOS, CORRECTED ALONGSIDE THIS: Micrium OS, not FreeRTOS. The FreeRTOS
 * event-group rewrite from the previous commit assumed the sanctioned RTOS
 * for ANY Bluetooth+RAIL DMP project on this SDK was FreeRTOS, based on the
 * bt_rail_dmp_soc_empty example (which does offer independent FreeRTOS and
 * Micrium OS project variants). That does not generalize: this project's own
 * manifest (rail_bt_dmp_soc_range_test.slcp) selects the RTOS by silicon
 * series --
 *     id: micriumos_kernel, condition: [device_series_2]
 *     id: freertos,         condition: [device_series_3]
 * -- and the EFR32xG28 is Series 2, so Studio generated this project against
 * Micrium OS, with no FreeRTOS option available for this exact chip in this
 * exact example. Confirmed by reading the real .slcp, not inferred. The wait
 * primitives below use OSFlagPend/OSFlagPost (event flags) and, in aps.c,
 * OSQPend/OSQPost/OSTaskCreate -- real signatures checked against
 * .../simplicity_sdk_2026.6.1/micriumos/platform/micrium_os/kernel/include/os.h
 * in this same project.
 *
 * Bring-up should call the generated sl_rail_util_init() (declared in
 * autogen/sl_rail_util_init.h) rather than reimplement sl_rail_init() +
 * sl_rail_config_channels() by hand -- it is Silicon Labs' own tested glue,
 * already wired to the real channel config, and retrieving the resulting
 * handle via sl_rail_util_get_handle() avoids duplicating what it already
 * does correctly.
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

/* The host protocol permits tuning throughout the 916 MHz band. The generated
 * Studio map is only a coarse startup map, so sl_subg_set_freq() installs a
 * one-channel runtime map at the requested frequency while retaining the
 * generated PHY and channel attributes.
 */
#define SL_SUBG_FREQ_MIN_HZ 914000000U
#define SL_SUBG_FREQ_MAX_HZ 918000000U

/* Matches the legacy firmware's 107-byte maximum Minimed RX payload and APS
 * encoding buffer. This is a capacity, not a fixed over-the-air TX length:
 * Minimed TX sends the actual encoded bytes plus a zero terminator. The RAIL
 * driver adjusts the configured fixed frame length per TX and restores this
 * 107-byte RX setting afterward.
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
 * The RAIL equivalent of that interrupt is SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL
 * (confirmed present in sl_rail_types.h this session, real generated project
 * -- see the file banner), delivered through the callback the generated init
 * code wires up -- so this function is implemented the same way: start_rx(),
 * drain on that event via sl_rail_read_rx_fifo(), terminate on 0x00,
 * sl_rail_idle() when done.
 */
enum sl_subg_rx_status sl_subg_get_pkt(uint8_t *buf, uint8_t *len,
				       uint32_t timeout_ms);

/** @brief Interrupt an in-progress sl_subg_get_pkt() from another context. */
void sl_subg_abort(void);
void sl_subg_clear_abort(void);

/**
 * @brief Last measured reply RSSI, dBm. Maps onto sl_rail_get_rssi() -- which
 * takes a microsecond wait_timeout parameter, not a bool, unlike the RAIL_*
 * family's RAIL_GetRssi() this replaced (see the file banner). Unlike the
 * RFM69 port, no first-byte-latch workaround should be needed, since RAIL
 * reports genuine per-packet RSSI rather than a live, freely drifting
 * register (see MIGRATION_NOTES.md section 21 on why that workaround existed
 * at all on the RFM69).
 */
int16_t sl_subg_get_last_rssi(void);

/**
 * @brief Set transmit power, in whole dBm.
 *
 * Grounded against the real sl_rail.h in this project's own copied SDK:
 * sl_rail_set_tx_power_dbm(handle, sl_rail_tx_power_t power_ddbm) takes
 * DECI-dBm (sl_rail_types.h: typedef int16_t sl_rail_tx_power_t). This
 * function takes whole dBm and multiplies by 10 -- unlike the RFM69 driver's
 * rf69_set_power_level(), which takes a raw PA register value (0-31), so
 * callers porting from subg.c cannot reuse the old constants
 * (SUBG_PA_LEVEL_MIN/DEFAULT) unchanged; see aps.c's equivalent call sites.
 * Requires TX power to have already been configured during
 * sl_rail_util_init() -- not independently confirmed against this exact
 * project's autogen output.
 */
int sl_subg_set_power_level(int16_t dbm);

/** @brief RX/TX packet counters, for CMD_GET_STATISTICS. Mirror
 * subg_get_rx_count()/subg_get_tx_count() in the RFM69 port.
 */
uint16_t sl_subg_get_rx_count(void);
uint16_t sl_subg_get_tx_count(void);

/**
 * @brief Tune to @p hz by registering a runtime single-channel RAIL map based
 * on the generated PHY. Takes effect on the next sl_subg_send_pkt() or
 * sl_subg_get_pkt() call.
 *
 * @return 0 if the runtime map was accepted, -1 if @p hz is outside the
 * supported band or RAIL rejected the map.
 */
int sl_subg_set_freq(uint32_t hz);

/** @brief Hz most recently requested through sl_subg_set_freq(), or the
 * default SL_SUBG_FREQ_HZ before the host requests a tune.
 */
uint32_t sl_subg_get_freq(void);

/** @brief Reset the runtime channel map to SL_SUBG_FREQ_HZ. Mirrors
 * rf69_config_916() as used by aps.c's CMD_RESET_RADIO_CFG.
 */
void sl_subg_reset_radio_cfg(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_SL_SUBG_RADIO_H_ */
