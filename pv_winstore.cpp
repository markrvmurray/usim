/* pv_winstore.cpp
 *
 * The simulator's parked-window store. On hardware these screens live in
 * PSRAM because SRAM has no room for them (win_store.h explains why); a host
 * has no such constraint, so this is simply an array.
 *
 * It exists because vid_console.cpp is a PORTABLE translation unit and must
 * stay that way: the firmware's win_store.cpp touches the QMI and cannot
 * compile here. Same seam as vid_link_irq_out() -- the portable code states
 * what it needs, each platform says how.
 *
 * The probe always succeeds. There is nothing here that can be absent, so
 * reporting a capability that must then be re-measured would be theatre.
 */

#include <stdio.h>

#include "win_store.h"
#include "pv_psram.h"

#define WIN_STORE_SIM_SLOTS  16u

static uint8_t slots[WIN_STORE_SIM_SLOTS][WIN_STORE_SLOT_BYTES];
static bool    absent;		/* the probe's verdict, as a test sets it */

void
pv_psram_set_absent(bool a)
{
	absent = a;
}

bool
win_store_init(void)
{
	return true;
}

unsigned
win_store_slots(void)
{
	return absent ? 0u : WIN_STORE_SIM_SLOTS;
}

uint8_t *
win_store_slot(unsigned i)
{
	return i < win_store_slots() ? slots[i] : NULL;
}

void
win_store_print_status(void)
{
	printf("Windows:     %u parked screens of %u B (simulated%s)\n",
	       win_store_slots(), WIN_STORE_SLOT_BYTES,
	       absent ? ", PSRAM absent" : "");
}
