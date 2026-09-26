/* pv_gfxshadow.cpp
 *
 * The simulator's graphics shadow. On hardware this is half a megabyte in
 * PSRAM, because SRAM has no room for a second plane; a host has no such
 * constraint, so it is an array.
 *
 * Same seam as the parked window screens: video_render.cpp is a PORTABLE
 * translation unit and states what storage it needs, and each platform says
 * where that storage comes from.
 */

#include "gfx_shadow.h"

/* Word-aligned explicitly: the renderer reads this plane as 32-bit words, and
 * a plain uint8_t array promises only byte alignment. */
alignas(4) static uint8_t shadow[GFX_SHADOW_BYTES];

uint8_t *
gfx_shadow_base(void)
{
	return shadow;
}
