/*
 * Local, fetch-after-the-fact diagnostic log.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * `commander rtt connect --noreset` repeatedly drops mid-session on this
 * board regardless of printf volume, EM2/EM3 deep sleep (ruled out --
 * still drops with EM1 forced), or blocking-vs-non-blocking RTT mode
 * (ruled out -- IOSTREAM_RTT_UP_MODE is already SEGGER_RTT_MODE_NO_BLOCK_TRIM,
 * so target execution can't stall on it either way). A live RTT session is
 * therefore not a reliable way to capture a full test run. This module
 * mirrors every dbg_printf() call into a fixed-size RAM ring buffer that
 * survives regardless of whether any debugger is attached, so a full log
 * only ever depends on one short, one-shot SWD memory read
 * (`commander readmem`) after the run -- not a connection that has to
 * survive the whole thing.
 */
#ifndef DBG_LOG_H
#define DBG_LOG_H

/* Zeroes the ring buffer and resets the write index. Call once, early in
 * app_init(), before anything else can call dbg_printf().
 */
void dbg_log_init(void);

/* Formats into the ring buffer (wrapping on overflow) and also best-effort
 * mirrors to the existing printf()/RTT path -- fine if RTT is disconnected
 * or drops it, the ring buffer is the real record.
 */
void dbg_printf(const char *fmt, ...);

#endif
