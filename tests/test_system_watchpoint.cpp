//
//	test_system_watchpoint.cpp
//	Unit tests for the SystemWatchpoint device. Exercises the device
//	APIs directly (no full simulator run), but uses a real mc6809
//	instance so the NMI OutputPin polarity convention matches the CPU
//	the device will be wired to.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>

#include "mc6809.h"
#include "system_watchpoint.h"

static int failures = 0;

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
				__FILE__, __LINE__, #cond);		\
			++failures;					\
		}							\
	} while (0)

static void test_disarmed_passthrough()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);

	// Fresh device: not armed, NMI deasserted (active-low pin reads true).
	CHECK(!w.armed());
	CHECK((bool)w.NMI == true);
	CHECK(!w.nmi_asserted());

	// Disarmed writes go straight to the shadow.
	w.vec_write(0x0E, 0xAB);	// $FFEE
	CHECK(w.vec_read(0x0E) == 0xAB);

	// load() seeds the shadow (used by firmware loaders).
	w.load(0xFFFE, 0x04);
	w.load(0xFFFF, 0x00);
	CHECK(w.vec_read(0x1E) == 0x04);	// $FFFE
	CHECK(w.vec_read(0x1F) == 0x00);	// $FFFF

	// load() outside the window is a no-op (doesn't crash).
	w.load(0x0000, 0xCC);
	CHECK(w.vec_read(0) != 0xCC);
}

static void test_arm_swaps_nmi_vector_and_copies_snippet()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);

	// Seed the NMI vector at $FFFC-$FFFD with a sentinel.
	w.load(0xFFFC, 0xCA);
	w.load(0xFFFD, 0xFE);

	// Arm via control register.
	w.ctrl_write(0x01);
	CHECK(w.armed());

	// NMI vector now points at $FFE0 (the snippet entry).
	CHECK(w.vec_read(0x1C) == 0xFF);	// $FFFC
	CHECK(w.vec_read(0x1D) == 0xE0);	// $FFFD

	// First snippet byte: LEAX 0,S = 0x30 0xE4.
	CHECK(w.vec_read(0x00) == 0x30);
	CHECK(w.vec_read(0x01) == 0xE4);

	// Re-arming is idempotent: doesn't double-save and clobber the
	// saved NMI vector.
	w.ctrl_write(0x02);
	CHECK(w.armed());
	CHECK(w.vec_read(0x1C) == 0xFF);
	CHECK(w.vec_read(0x1D) == 0xE0);
}

static void test_disarm_restores_nmi_vector()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);

	w.load(0xFFFC, 0xCA);
	w.load(0xFFFD, 0xFE);

	w.ctrl_write(0x01);
	CHECK(w.vec_read(0x1C) == 0xFF);	// vector swapped

	w.ctrl_write(0x00);
	CHECK(!w.armed());
	CHECK(w.vec_read(0x1C) == 0xCA);	// restored
	CHECK(w.vec_read(0x1D) == 0xFE);

	// Snippet bytes stay in the shadow (matches hardware semantics —
	// the spec says the Pico does not undo the snippet copy).
	CHECK(w.vec_read(0x00) == 0x30);
	CHECK(w.vec_read(0x01) == 0xE4);
}

static void test_armed_write_blocks_and_asserts_nmi()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);

	w.load(0xFFFC, 0xCA);
	w.load(0xFFFD, 0xFE);
	w.load(0xFFFE, 0x12);	// initial reset vector hi byte sentinel

	w.ctrl_write(0x01);
	CHECK((bool)w.NMI == true);		// not yet asserted

	// Attempted write to $FFFE while armed: blocked, NMI asserts.
	w.vec_write(0x1E, 0x99);
	CHECK(w.vec_read(0x1E) == 0x12);	// unchanged
	CHECK(w.nmi_asserted());
	CHECK((bool)w.NMI == false);		// active-low → asserted reads false

	// Spec: do NOT auto-disarm on trigger. NMI stays asserted; CPU
	// services on the falling edge, snippet runs (in a real run),
	// disarm happens only on explicit ctrl write or reset.
	CHECK(w.armed());

	// Disarm clears NMI and restores the vector.
	w.ctrl_write(0x00);
	CHECK(!w.armed());
	CHECK((bool)w.NMI == true);
	CHECK(w.vec_read(0x1C) == 0xCA);
	CHECK(w.vec_read(0x1D) == 0xFE);
}

static void test_disarmed_writes_still_work_after_trigger()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);

	w.load(0xFFFC, 0xCA);
	w.load(0xFFFD, 0xFE);

	w.ctrl_write(0x01);
	w.vec_write(0x1E, 0x99);		// blocked + NMI asserted
	w.ctrl_write(0x00);			// disarm

	// After disarm, writes go through again.
	w.vec_write(0x1E, 0x55);
	CHECK(w.vec_read(0x1E) == 0x55);
}

static void test_reset_disarms()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);

	w.load(0xFFFC, 0xCA);
	w.load(0xFFFD, 0xFE);

	w.ctrl_write(0x01);
	w.vec_write(0x1E, 0x99);		// trigger
	CHECK(w.armed());
	CHECK(w.nmi_asserted());

	w.reset();
	CHECK(!w.armed());
	CHECK(!w.nmi_asserted());
	CHECK(w.vec_read(0x1C) == 0xCA);	// restored
	CHECK(w.vec_read(0x1D) == 0xFE);
}

static void test_ctrl_read_is_zero()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);
	CHECK(w.ctrl_read() == 0);
	w.ctrl_write(0x01);
	CHECK(w.ctrl_read() == 0);		// write-only — read returns 0
}

static void test_adapter_devices_route_correctly()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);
	SystemWatchpointCtrl ctrl(w);
	SystemWatchpointVec vec(w);

	// Adapter route: offset arg is relative to attach base, so the
	// adapter passes through unchanged.
	vec.write(0x1F, 0x42);			// disarmed → goes to shadow
	CHECK(vec.read(0x1F) == 0x42);

	ctrl.write(0, 0x01);			// arm
	CHECK(w.armed());
	ctrl.write(0, 0x00);			// disarm
	CHECK(!w.armed());

	CHECK(ctrl.read(0) == 0);
}

int main()
{
	test_disarmed_passthrough();
	test_arm_swaps_nmi_vector_and_copies_snippet();
	test_disarm_restores_nmi_vector();
	test_armed_write_blocks_and_asserts_nmi();
	test_disarmed_writes_still_work_after_trigger();
	test_reset_disarms();
	test_ctrl_read_is_zero();
	test_adapter_devices_route_correctly();

	if (failures) {
		fprintf(stderr, "FAILED: %d assertion(s)\n", failures);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "OK\n");
	return EXIT_SUCCESS;
}
