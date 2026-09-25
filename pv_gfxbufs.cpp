/* pv_gfxbufs.cpp
 *
 * The simulator's get/put buffer pool. On hardware this is two megabytes of
 * PSRAM, because SRAM has none to spare; a host has no such constraint.
 *
 * Same seam as the parked window screens and the graphics shadow:
 * video_render.cpp is a PORTABLE translation unit and states what storage it
 * needs, and each platform says where that storage comes from.
 */

#include "gfx_bufs.h"

static uint8_t pool[GFX_BUFPOOL_BYTES];

uint8_t *
gfx_bufpool_base(void)
{
	return pool;
}
