/* pv_filllog.cpp
 *
 * The simulator's flood-fill span log. On hardware it lives in PSRAM; a host
 * has no such constraint, so it is an array. Same seam as the shadow, the
 * buffer pool and the command trace.
 */

#include "fill_log.h"
#include "win_store.h"

static uint32_t spans[FILL_LOG_SPANS];
static uint32_t seeds[FILL_SEEDS];

uint32_t *
fill_log_base(void)
{
	return win_store_slots() == 0u ? NULL : spans;
}

uint32_t *
fill_seeds_base(void)
{
	return win_store_slots() == 0u ? NULL : seeds;
}
