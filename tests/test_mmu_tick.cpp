//
//	test_mmu_tick.cpp
//	Unit tests for DATRAM (MMU), PicoTask (DAT task register), PicoTick
//	(50Hz timer). Exercises the device APIs directly without running
//	the simulator core.
//
//	Pico-thing-shaped expectations:
//	  - DATRAM identity-init: page table entry i = i, so a fresh DATRAM
//	    behaves as plain RAM for task 0.
//	  - PicoTask masks the written byte to 5 bits before forwarding to
//	    DATRAM::set_task (32 tasks).
//	  - PicoTick IRQ pin is inverted (invert=true): status bit 7 set
//	    means IRQ asserted on the bus, which surfaces as `bool(IRQ)
//	    == false` because the CPU sees active-low.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>

#include "memory.h"
#include "picotask.h"
#include "picotick.h"

static int failures = 0;

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
				__FILE__, __LINE__, #cond);		\
			++failures;					\
		}							\
	} while (0)

static void test_datram_identity_map()
{
	// 64KB guest visible window, 2MB physical, 256-byte page table.
	// Identity init: page N -> physical page N for every task.
	DATRAM dat(0xFE00, 2 * 1024 * 1024, 0x100);

	// write_physical at addr X should be visible through translated
	// read at X for task 0 (identity).
	dat.write_physical(0x1234, 0xAB);
	CHECK(dat.read(0x1234) == 0xAB);

	dat.write_physical(0xFDFF, 0xCD);
	CHECK(dat.read(0xFDFF) == 0xCD);

	// Reads of the page table window itself ($FE00 + i) return entry i.
	// Identity init -> datram[0] == 0, datram[1] == 1, ...
	CHECK(dat.read(0xFE00) == 0x00);
	CHECK(dat.read(0xFE01) == 0x01);
	CHECK(dat.read(0xFE10) == 0x10);
	CHECK(dat.read(0xFEFF) == 0xFF);
}

static void test_datram_set_task_and_paging()
{
	DATRAM dat(0xFE00, 2 * 1024 * 1024, 0x100);

	// Stash distinct sentinels at physical pages 0 and 32.
	// Page 0 base = 0x0000; page 32 base = 32 * 0x2000 = 0x40000.
	dat.write_physical(0x0000, 0x11);
	dat.write_physical(0x40000, 0x22);

	// Task 0, page 0 entry is 0 by identity init -> sees 0x11.
	CHECK(dat.get_task() == 0);
	CHECK(dat.read(0x0000) == 0x11);

	// Remap task 0's page 0 to physical page 32 by writing the page
	// table entry through the $FE00 window. Page table layout:
	// offset = (task << 3) | (guest_page).
	dat.write(0xFE00 | (0 << 3) | 0, 32);
	CHECK(dat.read(0x0000) == 0x22);

	// Switching task changes the active 8 entries.
	// Task 1 page 0 entry: offset (1 << 3) | 0 = 0x08. Identity init
	// left this as 0x08 -> physical page 8.
	dat.write_physical(8 * 0x2000, 0x33);
	dat.set_task(1);
	CHECK(dat.get_task() == 1);
	CHECK(dat.read(0x0000) == 0x33);

	// set_task masks to 5 bits.
	dat.set_task(0xFF);
	CHECK(dat.get_task() == 0x1F);
}

static void test_picotask_register()
{
	DATRAM dat(0xFE00, 2 * 1024 * 1024, 0x100);
	PicoTask reg(dat);

	// Write-only: reads return $FF, writes forward to DATRAM::set_task.
	CHECK(reg.read(0) == 0xFF);

	reg.write(0, 0x05);
	CHECK(dat.get_task() == 0x05);

	reg.write(0, 0xFF);
	CHECK(dat.get_task() == 0x1F);	// masked to 5 bits
}

static void test_picotick_disabled_by_default()
{
	PicoTick tick(100);
	tick.reset();

	// Disabled timer doesn't fire.
	for (int i = 0; i < 10; i++)
		tick.tick(20);

	// status read returns ctrl byte at offset 0; offset 1 is status.
	CHECK(tick.read(0) == 0x00);	// ctrl: disabled
	CHECK(tick.read(1) == 0x00);	// status: no IRQ pending

	// IRQ pin is active-low (invert=true): status bit 7 clear -> pin true.
	CHECK((bool)tick.IRQ == true);
}

static void test_picotick_fires_at_threshold()
{
	const uint32_t threshold = 100;
	PicoTick tick(threshold);
	tick.reset();

	tick.write(0, 0x01);	// enable
	CHECK(tick.read(0) == 0x01);

	// 50 cycles in: not yet at threshold.
	tick.tick(50);
	CHECK((bool)tick.IRQ == true);	// still not asserted

	// 50 more crosses the threshold: status bit 7 set, pin pulled low.
	tick.tick(50);
	CHECK((bool)tick.IRQ == false);	// asserted (active-low)

	// Reading the status register clears the pending bit.
	Byte s = tick.read(1);
	CHECK((s & 0x80) != 0);		// caller observes the pending bit
	CHECK((bool)tick.IRQ == true);	// pin deasserted after read
	CHECK(tick.read(1) == 0x00);	// status re-read: no longer pending
}

static void test_picotick_counter_wraps_not_resets()
{
	// Cycles past threshold roll over into the next period rather
	// than being discarded, so two thresholds don't drift relative
	// to wall time.
	PicoTick tick(100);
	tick.reset();
	tick.write(0, 0x01);

	tick.tick(150);				// 50 cycles over threshold
	CHECK((bool)tick.IRQ == false);		// fired
	(void)tick.read(1);			// clear

	tick.tick(60);				// 50 (carry) + 60 = 110, crosses again
	CHECK((bool)tick.IRQ == false);
	(void)tick.read(1);

	tick.tick(80);				// 10 carry + 80 = 90, not yet
	CHECK((bool)tick.IRQ == true);
}

static void test_picotick_disable_clears_pending()
{
	PicoTick tick(100);
	tick.reset();
	tick.write(0, 0x01);
	tick.tick(200);
	CHECK((bool)tick.IRQ == false);		// pending

	tick.write(0, 0x00);			// disable
	CHECK(tick.read(1) == 0x00);		// pending cleared on disable
	CHECK((bool)tick.IRQ == true);
}

static void test_picotick_reset_clears_state()
{
	PicoTick tick(100);
	tick.write(0, 0x01);
	tick.tick(200);				// fire

	tick.reset();
	CHECK(tick.read(0) == 0x00);		// ctrl cleared
	CHECK(tick.read(1) == 0x00);		// status cleared
	CHECK((bool)tick.IRQ == true);
}

int main()
{
	test_datram_identity_map();
	test_datram_set_task_and_paging();
	test_picotask_register();
	test_picotick_disabled_by_default();
	test_picotick_fires_at_threshold();
	test_picotick_counter_wraps_not_resets();
	test_picotick_disable_clears_pending();
	test_picotick_reset_clears_state();

	if (failures) {
		fprintf(stderr, "FAILED: %d assertion(s)\n", failures);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "OK\n");
	return EXIT_SUCCESS;
}
