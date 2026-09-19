/*
 * RileyLink APS command handler ("subg_rfspy 2.2") -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Port of orangelink-ncs:feather-nrf52832 src/aps/aps.c onto the xG28 RAIL
 * driver. See docs/aps-protocol-spec.md in that repo for the wire format --
 * it is reproduced here exactly; the companion app depends on it and none of
 * this port changes it.
 *
 * WHAT ACTUALLY CHANGED FROM THE SOURCE, vs. what is a straight port:
 *
 * 1. Radio calls: rf69_send_pkt-family calls -> sl_subg_* (src/drivers/rail/
 *    sl_subg_radio.h). subg_get_rx_count()/subg_get_tx_count() -> the counters
 *    now live in the RAIL driver itself (sl_subg_get_rx_count/tx_count),
 *    since this project has no separate subg.c -- sl_subg_radio.c already
 *    plays that role; see its header banner for why it isn't shaped like the
 *    RFM69 driver.
 * 2. rf69_set_freq()/rf69_get_freq() -> sl_subg_set_freq()/sl_subg_get_freq(),
 *    which map a requested Hz onto the nearest RAIL channel of the static
 *    channel config rather than retuning a synthesizer register -- see the
 *    comment on sl_subg_set_freq() in the driver header. rf69_config_916() ->
 *    sl_subg_reset_radio_cfg(), which resets channel selection only, since
 *    RAIL's PHY config is fixed at build time by the Radio Configurator, not
 *    reloaded at runtime.
 * 3. ips_send_response() -> aps_transport_send() (aps_transport.h). This
 *    project has no BLE/DMP layer yet; the transport is a narrow interface so
 *    this file compiles and its command parsing is testable before BLE
 *    exists, matching the driver's own "compile-clean, not hardware-run yet"
 *    status (see README.md).
 * 4. DISPATCH MODEL: the source runs a dedicated Zephyr thread
 *    (K_THREAD_STACK_DEFINE/k_thread_create) reading a depth-4 k_msgq,
 *    specifically so a blocking CMD_SEND_AND_LISTEN or CMD_GET_PKT (up to a
 *    client-supplied timeout, seconds) does not stall whatever runs the BLE
 *    stack. Ported here as a Micrium OS task + OS_Q of the same depth -- the
 *    RTOS for this project, corrected from an earlier FreeRTOS assumption
 *    after generating a real rail_bt_dmp_soc_range_test project for BRD2705A
 *    and reading its .slcp (device_series_2 -> micriumos_kernel; see the RTOS
 *    note in sl_subg_radio.c for the full trail). Micrium OS's OSQPost()
 *    posts a POINTER, not a value copy (unlike FreeRTOS's xQueueSend(), which
 *    the previous version relied on to copy struct aps_req by value) -- so
 *    aps_put_cmd() below copies into one of APS_QUEUE_DEPTH static pool slots
 *    and posts a pointer to that slot, sized to exactly match the queue depth
 *    so a slot is never reused while still queued. aps_put_cmd() still
 *    enqueues non-blocking and returns immediately (OSQPost() never blocks
 *    the poster in Micrium OS, matching the source's K_NO_WAIT and the
 *    property this whole design exists to preserve: the BLE GATT write
 *    callback, once that layer exists, is not blocked for the duration of a
 *    long listen). CMD_UPDATE_REG still runs inline rather than through the
 *    queue, exactly as in the source -- it never went through the queue
 *    there either.
 * 5. Byte-order and logging helpers (sys_get_be16/32, sys_put_be16/32, LOG_*)
 *    are Zephyr (zephyr/sys/byteorder.h, zephyr/logging/log.h) and are
 *    replaced with small local equivalents below rather than pulled in from
 *    anywhere, since this is not a Zephyr build.
 *
 * Everything else -- command parsing, the overflow bounds check, the
 * deferred-frequency-write reasoning, the send-and-listen retry loop, the
 * "legacy has no response for CMD_RESET" behaviour -- is carried over
 * unchanged, including the comments explaining why, since none of that
 * reasoning is RFM69-specific.
 */

#include <string.h>

#include "aps.h"
#include "aps_transport.h"
#include "drivers/rail/sl_subg_radio.h"
#include "4b6b.h"
#include "manchester.h"

#include "os.h"
#include "rtos_err.h"

/* ------------------------------------------------------------------------- *
 * Byte order and logging -- local replacements for Zephyr's
 * zephyr/sys/byteorder.h and zephyr/logging/log.h. See item 5 above.
 * ------------------------------------------------------------------------- */

static inline uint16_t aps_get_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t aps_get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline void aps_put_be16(uint16_t v, uint8_t *p)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void aps_put_be32(uint32_t v, uint8_t *p)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* No logging backend wired up yet (see item 5) -- these are no-ops so the
 * ported call sites below don't need editing again once one exists; swap the
 * bodies for the real thing (RTT, UART, whatever this project ends up using).
 */
#define APS_LOG_INF(...) ((void)0)
#define APS_LOG_WRN(...) ((void)0)
#define APS_LOG_ERR(...) ((void)0)
#define APS_LOG_DBG(...) ((void)0)
#define APS_LOG_HEXDUMP_INF(...) ((void)0)

/* ------------------------------------------------------------------------- *
 * Protocol constants
 * ------------------------------------------------------------------------- */

#define APS_SW_VERSION "subg_rfspy 2.2"
#define APS_STATE_OK   "OK"

/* Only the 916 MHz Minimed band is in scope. The legacy firmware also accepted
 * 866-870 (Minimed WWL) and 431-435 (Omnipod); those radios are not fitted.
 * An out-of-band frequency is logged and discarded, leaving the radio on its
 * previous setting -- legacy behaviour, preserved.
 */
#define APS_FREQ_916_MIN 914000000U
#define APS_FREQ_916_MAX 918000000U

/* RFM69/RAIL crystal used by the CC111x-compatible register encoding the host
 * sends. NOT the radio's own crystal -- carried over unchanged from the RFM69
 * port, since this is the host protocol's own frequency encoding, not
 * anything specific to the radio underneath.
 */
#define APS_RILEYLINK_FXOSC 24000000ULL

enum aps_cmd {
	CMD_GET_STATE       = 0x01,
	CMD_GET_VER         = 0x02,
	CMD_GET_PKT         = 0x03,
	CMD_SEND_PKT        = 0x04,
	CMD_SEND_AND_LISTEN = 0x05,
	CMD_UPDATE_REG      = 0x06,
	CMD_RESET           = 0x07,
	CMD_LED             = 0x08,
	CMD_READ_REG        = 0x09,
	CMD_SET_MODE_REG    = 0x0A,
	CMD_SET_SW_ENCODING = 0x0B,
	CMD_SET_PREAMBLE    = 0x0C,
	CMD_RESET_RADIO_CFG = 0x0D,
	CMD_GET_STATISTICS  = 0x0E,
};

enum aps_encoding {
	ENCODING_NONE = 0,
	ENCODING_MANCHESTER = 1,
	ENCODING_4B6B = 2,
};

struct aps_req {
	uint8_t cmd;
	int8_t rssi;
	uint16_t len;
	uint8_t param[APS_MAX_PARAM_LEN];
};

/* ------------------------------------------------------------------------- *
 * State
 * ------------------------------------------------------------------------- */

static enum aps_encoding encoding = ENCODING_NONE;
static uint8_t freq_reg[3] = { 0x12, 0x14, 0x83 };  /* legacy default: 433.92 MHz */

/*
 * Set when the host changes a frequency register; applied to the radio just
 * before the next radio command. See the source's extensive note on why this
 * is deferred rather than applied inline (a burst scattered across three
 * frequencies as AndroidAPS writes three registers mid-transmit) -- unchanged
 * reasoning, carried over verbatim since it has nothing to do with which
 * radio is underneath.
 */
static bool freq_pending;
static uint8_t use_pkt_len;
static bool active;

static uint32_t loop_count;   /* drives the statistics updTime field */

/* ------------------------------------------------------------------------- *
 * Response helpers
 * ------------------------------------------------------------------------- */

/* Bare status byte, e.g. an error code. */
static void respond_code(uint8_t code)
{
	aps_transport_send(&code, 1);
}

/* Data-bearing response: [0xDD][payload...]. */
static void respond_data(const uint8_t *data, uint16_t len)
{
	uint8_t buf[APS_RESP_MAX_LEN];

	if (len > sizeof(buf) - 1) {
		APS_LOG_WRN("response %u B truncated to %u", len, (unsigned)sizeof(buf) - 1);
		len = sizeof(buf) - 1;
	}

	buf[0] = APS_RESP_SUCCESS;
	memcpy(&buf[1], data, len);
	aps_transport_send(buf, len + 1);
}

/* ------------------------------------------------------------------------- *
 * Encoding
 * ------------------------------------------------------------------------- */

static uint16_t encode(const uint8_t *src, uint8_t *dst, uint16_t len)
{
	switch (encoding) {
	case ENCODING_NONE:
		memcpy(dst, src, len);
		return len;
	case ENCODING_MANCHESTER:
		return encode_manchester(src, dst, len) ? len * 2 : 0;
	case ENCODING_4B6B:
		return encode_4b6b(src, dst, len);
	default:
		return 0;
	}
}

static uint16_t decode(const uint8_t *src, uint8_t *dst, uint16_t len)
{
	switch (encoding) {
	case ENCODING_NONE:
		memcpy(dst, src, len);
		return len;
	case ENCODING_MANCHESTER:
		return decode_manchester(src, dst, len);
	case ENCODING_4B6B:
		return decode_4b6b(src, dst, len);
	default:
		return 0;
	}
}

/* ------------------------------------------------------------------------- *
 * Frequency
 * ------------------------------------------------------------------------- */

static uint32_t freq_from_regs(void)
{
	uint32_t reg = ((uint32_t)freq_reg[0] << 16) |
		       ((uint32_t)freq_reg[1] << 8) | freq_reg[2];

	return (uint32_t)(((uint64_t)reg * APS_RILEYLINK_FXOSC) >> 16);
}

/* Called immediately before a radio command -- see the DISPATCH MODEL note at
 * the top of this file for why "before a radio command" and "from the
 * transport callback" are no longer different contexts here.
 */
static void apply_pending_freq(void)
{
	uint32_t hz;

	if (!freq_pending) {
		return;
	}
	freq_pending = false;

	hz = freq_from_regs();

	if (hz < APS_FREQ_916_MIN || hz > APS_FREQ_916_MAX) {
		/* Legacy logged and discarded, leaving the radio unchanged. Kept --
		 * but note this build only carries the 916 MHz radio, so a host
		 * asking for 433 or 868 gets silently ignored rather than retuned.
		 */
		APS_LOG_WRN("frequency %u Hz outside the 916 MHz band, ignored", hz);
		return;
	}

	if (sl_subg_set_freq(hz) != 0) {
		APS_LOG_ERR("sl_subg_set_freq failed (outside channel map)");
		return;
	}

	/*
	 * Read the frequency back rather than trusting the write -- carried over
	 * from a real bug on the RFM69 port (a HackRF capture caught the radio
	 * transmitting on the config-table default while this function logged
	 * every frequency AndroidAPS asked for; the tune was computed and logged
	 * but never took effect). sl_subg_set_freq() snaps to the nearest channel
	 * rather than tuning a synthesizer register, so this now also catches
	 * rounding to a channel further away than intended.
	 */
	{
		uint32_t actual = sl_subg_get_freq();
		int32_t err_hz = (int32_t)(actual - hz);

		if (err_hz > 5000 || err_hz < -5000) {
			APS_LOG_ERR("tune FAILED: asked %u Hz, radio reads %u Hz (%+d)",
				    hz, actual, err_hz);
		} else {
			APS_LOG_INF("tuned to %u Hz (radio reads %u)", hz, actual);
		}
	}
}

/* ------------------------------------------------------------------------- *
 * Commands
 * ------------------------------------------------------------------------- */

static void cmd_get_state(void)
{
	respond_data((const uint8_t *)APS_STATE_OK, strlen(APS_STATE_OK));
}

static void cmd_get_version(void)
{
	respond_data((const uint8_t *)APS_SW_VERSION, strlen(APS_SW_VERSION));
}

static void cmd_set_sw_encoding(const uint8_t *p, uint16_t len)
{
	if (len < 1) {
		return;   /* legacy: no response on a short frame */
	}

	switch (p[0]) {
	case ENCODING_NONE:
	case ENCODING_MANCHESTER:
	case ENCODING_4B6B:
		encoding = (enum aps_encoding)p[0];
		APS_LOG_INF("encoding set to %u", p[0]);
		respond_code(APS_RESP_SUCCESS);
		break;
	default:
		respond_code(APS_RESP_PARAM_ERROR);
		break;
	}
}

static void cmd_update_reg(const uint8_t *p, uint16_t len)
{
	uint8_t addr, value;

	/* AndroidAPS sends 2 bytes, Loop sends 10; only the first two are read.
	 * Legacy returned WITHOUT any response on a short frame -- preserved, so
	 * the client times out exactly as before.
	 */
	if (len < 2) {
		APS_LOG_WRN("CMD_UPDATE_REG len %u < 2, no response", len);
		return;
	}

	addr = p[0];
	value = p[1];

	switch (addr) {
	case 0x02:
		use_pkt_len = 1;
		APS_LOG_INF("fixed RX payload length = %u", value);
		break;
	case 0x09:
	case 0x0A:
	case 0x0B:
		/* Record only. Applied just before the next radio command, so a
		 * change can never land mid-burst.
		 */
		freq_reg[addr - 0x09] = value;
		freq_pending = true;
		break;
	case 0x0C:
		/* Legacy switched to MINIMED_WWL (868 MHz) on 0x59. That radio is not
		 * fitted here, so the 916 configuration is reapplied instead -- and
		 * deferred, for the same reason as the frequency registers.
		 */
		if (value == 0x59) {
			APS_LOG_INF("host requested Minimed mode; will reapply 916 config");
			freq_pending = true;
		}
		break;
	default:
		APS_LOG_DBG("CMD_UPDATE_REG addr 0x%02x ignored", addr);
		break;
	}

	respond_code(APS_RESP_SUCCESS);
}

static void cmd_read_reg(const uint8_t *p, uint16_t len)
{
	uint8_t value;

	if (len < 1) {
		APS_LOG_WRN("CMD_READ_REG len 0, no response");
		return;
	}

	switch (p[0]) {
	case 0x09:
	case 0x0A:
	case 0x0B:
		value = freq_reg[p[0] - 0x09];
		break;
	default:
		/* Legacy stub: every other address returns 0x5A. Not a real read. */
		value = 0x5A;
		break;
	}

	respond_data(&value, 1);
}

static void cmd_set_preamble(const uint8_t *p, uint16_t len)
{
	if (len < 2) {
		return;
	}
	/* Big-endian on the wire. Not otherwise acted on -- matches legacy, which
	 * also only logged it. APS_LOG_INF is a no-op until a real logging
	 * backend exists (see item 5 in the file banner), so p would otherwise be
	 * an unused parameter; aps_get_be16() is still called for its own sake so
	 * this keeps parsing the field even while nothing consumes the value.
	 */
	(void)aps_get_be16(p);
	APS_LOG_INF("preamble = %u", aps_get_be16(p));
	respond_code(APS_RESP_SUCCESS);
}

static void cmd_reset_radio_cfg(void)
{
	sl_subg_reset_radio_cfg();
	encoding = ENCODING_NONE;
	respond_code(APS_RESP_SUCCESS);
}

static void cmd_get_statistics(void)
{
	/* 20 bytes, all multi-byte fields big-endian. Field order is part of the
	 * wire format -- see docs/aps-protocol-spec.md section 3 in the source repo.
	 */
	uint8_t buf[20] = { 0 };

	aps_put_be32(loop_count * 10, &buf[0]);   /* updTime, ms */
	aps_put_be16(0, &buf[4]);                 /* rxOverflowCnt      (always 0) */
	aps_put_be16(0, &buf[6]);                 /* rxFifoOverflowCnt  (always 0) */
	aps_put_be16(sl_subg_get_rx_count(), &buf[8]);
	aps_put_be16(sl_subg_get_tx_count(), &buf[10]);
	aps_put_be16(0, &buf[12]);                /* crcFailCnt         (always 0) */
	aps_put_be16(0, &buf[14]);                /* spiSyncFailCnt     (always 0) */
	aps_put_be16(0, &buf[16]);                /* placeholder0 */
	aps_put_be16(0, &buf[18]);                /* placeholder1 */

	respond_data(buf, sizeof(buf));
}

/* ------------------------------------------------------------------------- *
 * Radio commands
 *
 * Parameters are parsed with aps_get_be16/32 rather than by overlaying a
 * packed struct and byte-swapping in place -- the legacy code took the
 * address of unaligned packed members and cast them, which is undefined
 * behaviour. See docs/aps-protocol-spec.md section 6.3 in the source repo.
 * ------------------------------------------------------------------------- */

/* RileyLink clients expect CC111x-style RSSI. Legacy formula, truncation included. */
static uint8_t rssi_to_cc111x(int16_t dbm)
{
	return (uint8_t)((dbm + 73) * 2);
}

/* Emit [0xDD][rssi][pktCnt][payload...] for a received packet. */
static void respond_rx_packet(const uint8_t *pkt, uint16_t len)
{
	uint8_t buf[2 + SL_SUBG_MAX_PKT_LEN];

	if (len > SL_SUBG_MAX_PKT_LEN) {
		len = SL_SUBG_MAX_PKT_LEN;
	}

	buf[0] = rssi_to_cc111x(sl_subg_get_last_rssi());
	buf[1] = (uint8_t)(sl_subg_get_rx_count() & 0xFF);
	memcpy(&buf[2], pkt, len);

	respond_data(buf, len + 2);
}

static void respond_rx_status(enum sl_subg_rx_status st, const uint8_t *pkt, uint8_t len)
{
	switch (st) {
	case SL_SUBG_RX_OK:
		respond_rx_packet(pkt, len);
		break;
	case SL_SUBG_RX_TIMEOUT:
		respond_code(APS_RESP_RX_TIMEOUT);
		break;
	case SL_SUBG_RX_INTERRUPTED:
		respond_code(APS_RESP_CMD_INTERRUPTED);
		break;
	}
}

/* CMD_GET_PKT: [listenChan][listenTimeout BE32] */
static void cmd_get_pkt(const uint8_t *p, uint16_t len)
{
	apply_pending_freq();
	uint8_t raw[SL_SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t dec[SL_SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t raw_len = 0;
	uint16_t dec_len;
	uint32_t timeout;
	enum sl_subg_rx_status st;

	if (len < 5) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	timeout = aps_get_be32(&p[1]);   /* p[0] is listenChan, accepted and unused */

	st = sl_subg_get_pkt(raw, &raw_len, timeout);
	if (st != SL_SUBG_RX_OK) {
		APS_LOG_INF("send+listen: no reply (status %d)", st);
		respond_rx_status(st, NULL, 0);
		return;
	}

	dec_len = decode(raw, dec, raw_len);
	APS_LOG_INF("send+listen: REPLY %u B raw -> %u B decoded", raw_len, dec_len);
	APS_LOG_HEXDUMP_INF(dec, dec_len, "pump reply");
	respond_rx_packet(dec, dec_len);
}

/* Strip one trailing zero byte, as the legacy TX path did for Minimed. The radio
 * layer appends its own terminator, so a caller-supplied one is redundant.
 */
static uint16_t trim_trailing_zero(const uint8_t *pkt, uint16_t len)
{
	if (len > 0 && pkt[len - 1] == 0) {
		return len - 1;
	}
	return len;
}

/* CMD_SEND_PKT: [sendChan][repeatCnt][repeatIntvl BE16][preambleExtend BE16][payload...] */
static void cmd_send_pkt(const uint8_t *p, uint16_t len)
{
	apply_pending_freq();
	uint8_t enc[SL_SUBG_MAX_PKT_LEN] = { 0 };
	uint16_t payload_len, enc_len;
	uint8_t repeat_cnt;
	uint16_t repeat_intvl;

	if (len < 6) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	repeat_cnt = p[1];
	repeat_intvl = aps_get_be16(&p[2]);
	payload_len = trim_trailing_zero(&p[6], len - 6);

	enc_len = encode(&p[6], enc, payload_len);
	if (enc_len == 0 || enc_len > sizeof(enc)) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	sl_subg_send_pkt(enc, (uint8_t)enc_len, repeat_cnt, repeat_intvl);
	respond_code(APS_RESP_SUCCESS);
}

/*
 * CMD_SEND_AND_LISTEN:
 * [sendChan][repeatCnt][repeatIntvl BE16][listenChan][listenTimeout BE32]
 * [retryCnt][preambleExtend BE16][payload...]
 */
static void cmd_send_and_listen(const uint8_t *p, uint16_t len)
{
	apply_pending_freq();
	uint8_t enc[SL_SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t raw[SL_SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t dec[SL_SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t raw_len = 0;
	uint16_t payload_len, enc_len, dec_len;
	uint8_t repeat_cnt, retry_cnt;
	uint16_t repeat_intvl;
	uint32_t timeout;
	enum sl_subg_rx_status st;

	if (len < 12) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	repeat_cnt = p[1];
	repeat_intvl = aps_get_be16(&p[2]);
	timeout = aps_get_be32(&p[5]);
	retry_cnt = p[9];
	payload_len = trim_trailing_zero(&p[12], len - 12);

	enc_len = encode(&p[12], enc, payload_len);
	if (enc_len == 0 || enc_len > sizeof(enc)) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	APS_LOG_INF("send+listen: %u B payload -> %u B encoded, listen %u ms, retries %u",
		    payload_len, enc_len, timeout, retry_cnt);

	sl_subg_send_pkt(enc, (uint8_t)enc_len, repeat_cnt, repeat_intvl);
	st = sl_subg_get_pkt(raw, &raw_len, timeout);

	/* Retry loop: resend with repeatCnt forced to 0, as in legacy. */
	while (st == SL_SUBG_RX_TIMEOUT && retry_cnt > 0) {
		APS_LOG_DBG("send-and-listen retry, %u left", retry_cnt);
		sl_subg_send_pkt(enc, (uint8_t)enc_len, 0, repeat_intvl);
		st = sl_subg_get_pkt(raw, &raw_len, timeout);
		retry_cnt--;
	}

	if (st != SL_SUBG_RX_OK) {
		APS_LOG_INF("send+listen: no reply (status %d)", st);
		respond_rx_status(st, NULL, 0);
		return;
	}

	dec_len = decode(raw, dec, raw_len);
	APS_LOG_INF("send+listen: REPLY %u B raw -> %u B decoded", raw_len, dec_len);
	APS_LOG_HEXDUMP_INF(dec, dec_len, "pump reply");
	respond_rx_packet(dec, dec_len);
}

/* ------------------------------------------------------------------------- *
 * Dispatch
 * ------------------------------------------------------------------------- */

static void aps_dispatch(const struct aps_req *req)
{
	switch (req->cmd) {
	case CMD_GET_STATE:
		cmd_get_state();
		break;
	case CMD_GET_VER:
		cmd_get_version();
		break;
	case CMD_GET_PKT:
		cmd_get_pkt(req->param, req->len);
		break;
	case CMD_SEND_PKT:
		cmd_send_pkt(req->param, req->len);
		break;
	case CMD_SEND_AND_LISTEN:
		cmd_send_and_listen(req->param, req->len);
		break;
	case CMD_UPDATE_REG:
		cmd_update_reg(req->param, req->len);
		break;
	case CMD_LED:
		/* Accepted and ignored, as in the legacy firmware. */
		respond_code(APS_RESP_SUCCESS);
		break;
	case CMD_READ_REG:
		cmd_read_reg(req->param, req->len);
		break;
	case CMD_SET_MODE_REG:
		/* Accepted and ignored. */
		respond_code(APS_RESP_SUCCESS);
		break;
	case CMD_SET_SW_ENCODING:
		cmd_set_sw_encoding(req->param, req->len);
		break;
	case CMD_SET_PREAMBLE:
		cmd_set_preamble(req->param, req->len);
		break;
	case CMD_RESET_RADIO_CFG:
		cmd_reset_radio_cfg();
		break;
	case CMD_GET_STATISTICS:
		cmd_get_statistics();
		break;
	case CMD_RESET:
		/* Legacy has NO case for 0x07: it falls through to default, is logged
		 * as unknown, and produces no response at all. The client times out.
		 * Reproduced deliberately -- changing it is a protocol change.
		 */
	default:
		APS_LOG_WRN("unknown command 0x%02x (no response, as in legacy)", req->cmd);
		break;
	}
}

/* ------------------------------------------------------------------------- *
 * Dispatch task -- see item 4 in the file banner.
 * ------------------------------------------------------------------------- */

/* Depth 4, as in the source's K_MSGQ_DEFINE(aps_msgq, sizeof(struct aps_req),
 * 4, 4) -- absorbs a three-write frequency burst (CMD_UPDATE_REG, though,
 * never actually enters this queue; see below) without dropping a queued
 * command. Stack size in CPU_STK units (Micrium OS's native stack element
 * type, not bytes) -- not yet tuned against a real measured high-water mark,
 * carried over as a reasonable starting guess from the source's 2048-BYTE
 * Zephyr stack.
 */
#define APS_TASK_STACK_SIZE_ELEMS 512
#define APS_TASK_PRIORITY 10   /* TODO: not yet verified against this
				 * project's real Bluetooth task priorities --
				 * must end up below (i.e. numerically GREATER
				 * than, in Micrium OS's convention where 0 is
				 * highest priority -- confirmed from os.h:
				 * OS_PRIO_INIT is defined as OS_CFG_PRIO_MAX,
				 * the "unassigned" sentinel, implying the
				 * numeric max is the least urgent end of the
				 * range) whatever priority the generated
				 * Bluetooth stack task(s) run at, mirroring the
				 * source's "priority below the Bluetooth RX
				 * thread" requirement (APS_THREAD_PRIORITY).
				 * This project's own task priorities were not
				 * read this session -- same discipline as the
				 * RAIL_* vs sl_rail_* question, needs checking
				 * against real generated/configured values
				 * before this number means anything.
				 */
#define APS_QUEUE_DEPTH 4

static CPU_STK s_aps_task_stack[APS_TASK_STACK_SIZE_ELEMS];
static OS_TCB s_aps_task_tcb;
static OS_Q s_aps_queue;

/* OSQPost() posts a POINTER, not a value copy (unlike FreeRTOS's
 * xQueueSend(), which the previous version of this file relied on) -- see
 * item 4 in the file banner. Sized to exactly APS_QUEUE_DEPTH so a pool slot
 * is never in use by more than one queued command at a time: the queue
 * itself (max_qty APS_QUEUE_DEPTH) guarantees no more than APS_QUEUE_DEPTH
 * posts are outstanding before a post is refused, which is exactly the pool
 * size.
 */
static struct aps_req s_aps_req_pool[APS_QUEUE_DEPTH];
static uint8_t s_aps_req_pool_next;

static void aps_task_fn(void *p_arg)
{
	RTOS_ERR err;

	(void)p_arg;

	while (true) {
		OS_MSG_SIZE msg_size;
		struct aps_req *req;

		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		req = (struct aps_req *)OSQPend(&s_aps_queue, 0, OS_OPT_PEND_BLOCKING,
						&msg_size, NULL, &err);
		if (RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE || req == NULL) {
			continue;
		}
		/* Source's Subg_ClrIntFlg(): clear any stale preemption request
		 * once, here, before the command runs -- must happen from this
		 * task, immediately before dispatch, not from aps_put_cmd()'s
		 * caller context, or an abort arriving between enqueue and
		 * dequeue would be cleared before the command it was meant to
		 * interrupt ever runs.
		 */
		sl_subg_clear_abort();
		loop_count++;
		aps_dispatch(req);
	}
}

/* ------------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------------- */

void aps_put_cmd(const uint8_t *buf, uint16_t len, int8_t rssi)
{
	uint16_t param_len;

	if (buf == NULL || len == 0) {
		return;
	}

	/* Legacy self-consistency check: byte 0 counts everything after itself. */
	if (len == 1 || buf[0] != len - 1) {
		APS_LOG_DBG("malformed frame (len %u, byte0 0x%02x), dropped", len, buf[0]);
		return;
	}

	param_len = len - 2;

	/*
	 * BOUNDS CHECK -- carried over from the source's fix for a real overflow
	 * (docs/aps-protocol-spec.md section 7 there). Legacy Aps_PutCmd() did:
	 *     req.pktLen = len - 2;
	 *     memcpy(req.pkt, pBuf + 2, req.pktLen);
	 * with req.pkt only 123 bytes and len up to 150 from the Data
	 * characteristic -- a 25-byte stack overflow reachable over an
	 * unauthenticated BLE link.
	 */
	if (param_len > APS_MAX_PARAM_LEN) {
		APS_LOG_WRN("frame params %u B exceed %u, rejected",
			    param_len, APS_MAX_PARAM_LEN);
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	if (!active) {
		APS_LOG_DBG("APS inactive, frame dropped");
		return;
	}

	/* CMD_UPDATE_REG runs inline in the source too (never queued), so this
	 * needs no adjustment for the synchronous dispatch model here.
	 */
	if (buf[1] == CMD_UPDATE_REG) {
		cmd_update_reg(&buf[2], param_len);
		return;
	}

	{
		struct aps_req *slot = &s_aps_req_pool[s_aps_req_pool_next];
		RTOS_ERR err;

		s_aps_req_pool_next = (uint8_t)((s_aps_req_pool_next + 1) % APS_QUEUE_DEPTH);

		slot->cmd = buf[1];
		slot->rssi = rssi;
		slot->len = param_len;
		memcpy(slot->param, &buf[2], param_len);

		/* Non-blocking, matching the source's k_msgq_put(..., K_NO_WAIT):
		 * OSQPost() never blocks the poster in Micrium OS (unlike
		 * OSQPend()) -- a full queue simply returns an error rather than
		 * blocking the caller (the BLE GATT write callback, once that
		 * layer exists). Losing a command to a full queue is preferable
		 * to stalling the link.
		 */
		err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
		OSQPost(&s_aps_queue, slot, sizeof(*slot), OS_OPT_POST_FIFO, &err);
		if (RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE) {
			APS_LOG_DBG("busy, command 0x%02x dropped", slot->cmd);
		}
	}
}

void aps_set_active(bool on)
{
	if (!on) {
		/* Legacy aborted an in-flight receive when BLE dropped to advertising,
		 * which is what produced RESPONSE_CODE_CMD_INTERRUPTED. Preserved.
		 */
		sl_subg_abort();
	}
	active = on;
	APS_LOG_INF("APS %s", on ? "active" : "inactive");
}

void aps_init(void)
{
	RTOS_ERR err;

	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
	OSQCreate(&s_aps_queue, "aps cmd queue", APS_QUEUE_DEPTH, &err);

	err = (RTOS_ERR)RTOS_ERR_INIT_CODE(RTOS_ERR_NONE);
	OSTaskCreate(&s_aps_task_tcb, "aps", aps_task_fn, NULL, APS_TASK_PRIORITY,
		    s_aps_task_stack, APS_TASK_STACK_SIZE_ELEMS / 10,
		    APS_TASK_STACK_SIZE_ELEMS, 0, 0, NULL,
		    OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR, &err);

	/* sl_subg_radio_init() is the caller's responsibility, same as the source
	 * (subg_init() is called separately from wherever board init happens),
	 * not duplicated here.
	 */
	APS_LOG_INF("APS ready (%s)", APS_SW_VERSION);
}
