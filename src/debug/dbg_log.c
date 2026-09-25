/*
 * Local, fetch-after-the-fact diagnostic log -- see dbg_log.h for why this
 * exists instead of relying on a live RTT connection.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "dbg_log.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "printf.h"
#include "sl_core.h"

#define DBG_LOG_BUF_SIZE (16U * 1024U)

/* All four fields below are read out of a live memory dump after the test
 * run (commander readmem at this symbol's address, from the .map file --
 * no fixed/linker-scripted address needed since the whole point is a
 * one-shot read, not something firmware itself has to locate at runtime).
 * total_written is monotonic and never wraps -- the host script uses
 * (total_written - buf_size) as the true start offset once
 * total_written > buf_size, so wraparound is unambiguous to reconstruct.
 * boot_count lets the host tell whether the device reset between two
 * captures it's comparing.
 */
__attribute__((used)) static volatile uint32_t s_dbg_log_magic = 0x474F4C32U; /* "2LOG" */
__attribute__((used)) static volatile uint32_t s_dbg_log_boot_count;
__attribute__((used)) static volatile uint32_t s_dbg_log_total_written;
__attribute__((used)) static char s_dbg_log_buf[DBG_LOG_BUF_SIZE];

static void dbg_log_putc(char character, void *arg)
{
	CORE_DECLARE_IRQ_STATE;

	(void)arg;
	CORE_ENTER_ATOMIC();
	s_dbg_log_buf[s_dbg_log_total_written % DBG_LOG_BUF_SIZE] = character;
	s_dbg_log_total_written++;
	CORE_EXIT_ATOMIC();
}

void dbg_log_init(void)
{
	memset((void *)s_dbg_log_buf, 0, sizeof(s_dbg_log_buf));
	s_dbg_log_total_written = 0;
	s_dbg_log_boot_count++;
}

void dbg_printf(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	(void)vfctprintf(dbg_log_putc, NULL, fmt, va);
	va_end(va);

	/* Best-effort live mirror over the existing printf()/RTT path -- see
	 * this file's header comment on why this is not the source of truth.
	 */
	va_start(va, fmt);
	(void)vprintf(fmt, va);
	va_end(va);
}
