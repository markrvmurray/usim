/* pv_cmdtrace.cpp
 *
 * The simulator's command trace. On hardware it lives in PSRAM, because SRAM
 * has no room; a host has no such constraint, so it is an array. Same seam as
 * the graphics shadow and the buffer pool: the portable link says what storage
 * it needs, and each platform says where it comes from.
 */

#include "cmd_trace.h"

alignas(4) static uint8_t ring[CMD_TRACE_BYTES];

uint8_t *
cmd_trace_base(void)
{
	return ring;
}
