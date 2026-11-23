//
//	test_peek.cpp
//	A debugger or monitor looking at a device must not change it.
//	For each device whose reads have side effects, peek() returns
//	what read() would and leaves the device as it was: the ACIA's
//	received byte stays pending, the tick timer's IRQ stays
//	unacknowledged, the IDE data register doesn't advance, and an
//	unmapped DAT page raises no fault. USim::debug_read goes through
//	peek(), so a debugger's memory read inherits all of this.
//
//	A debugger's write, poke(), reaches a device as a store does, but
//	is not the guest's: the DAT logs none of it and faults on none of
//	it, and the system watchpoint's armed window lets it land without
//	tripping NMI.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>

#include "mc6809.h"
#include "mc6850.h"
#include "memory.h"
#include "picoide.h"
#include "picotick.h"
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

// One byte of input, then nothing
struct OneByte : mc6850_impl {
	bool		sent = false;
	bool		poll_read() override { return !sent; }
	Byte		read() override { sent = true; return 'K'; }
	void		write(Byte) override {}
};

static void test_acia()
{
	OneByte in;
	mc6850 acia(in, 1);
	static_cast<ActiveDevice&>(acia).tick(1);
	CHECK(acia.read(0) & 0x01);		// RDRF

	CHECK(acia.peek(1) == 'K');
	CHECK(acia.peek(0) & 0x01);		// still pending
	CHECK(acia.read(1) == 'K');
	CHECK(!(acia.read(0) & 0x01));		// the CPU's read took it
}

static void test_tick()
{
	PicoTick tick(10);
	tick.reset();
	tick.write(0, 0x01);			// enable
	tick.tick(10);
	CHECK(tick.peek(1) & 0x80);
	CHECK(tick.peek(1) & 0x80);		// peeking doesn't acknowledge
	CHECK((bool)tick.IRQ == false);		// still asserted
	CHECK(tick.peek(0) == 0x01);
	CHECK(tick.read(1) & 0x80);
	CHECK(!(tick.read(1) & 0x80));		// reading does
}

static void test_ide()
{
	char path[] = "/tmp/peek_ide_XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		perror("mkstemp");
		exit(EXIT_FAILURE);
	}
	Byte sector[512];
	for (int i = 0; i < 512; i++)
		sector[i] = (Byte)i;
	if (write(fd, sector, sizeof sector) != (ssize_t)sizeof sector) {
		perror("write");
		exit(EXIT_FAILURE);
	}
	close(fd);

	{
		PicoIDE ide(path);
		ide.write(3, 1);		// one sector
		ide.write(4, 0);
		ide.write(5, 0);
		ide.write(6, 0);
		ide.write(7, 0xE0);		// LBA, drive 0
		ide.write(8, 0x20);		// READ SECTORS
		CHECK(ide.peek(8) & 0x08);	// DRQ

		// peeking the pair leaves the transfer where it was
		CHECK(ide.peek(0) == 0x00);
		CHECK(ide.peek(1) == 0x01);
		CHECK(ide.peek(1) == 0x01);
		CHECK(ide.read(0) == 0x00);
		CHECK(ide.read(1) == 0x01);
		CHECK(ide.peek(0) == 0x02);
		CHECK(ide.peek(1) == 0x03);
	}
	unlink(path);
}

static void test_dat()
{
	DATRAM dat(0xFE00, 2 * 1024 * 1024, 0x100);
	dat.set_fixed_window(0xE000, 0x1E00, 0x1E000);
	dat.write(0xFE00 + 0x00, 0xFF);		// task 0, page 0 unavailable

	CHECK(dat.peek(0x0000) == 0xFF);
	CHECK((bool)dat.NMI == true);		// no fault raised
	CHECK(dat.read(0x0000) == 0xFF);
	CHECK((bool)dat.NMI == false);		// the CPU's read faults
}

static void test_dat_poke()
{
	DATRAM dat(0xFE00, 2 * 1024 * 1024, 0x100);
	dat.set_fixed_window(0xE000, 0x1E00, 0x1E000);
	int logged = 0, stored = 0;
	dat.add_wlog_range(false, 0x0000, 0x10000);
	dat.set_wlog_cb([&](Word, size_t, Byte, Byte) { logged++; });
	dat.set_store_cb([&](Word, size_t, Byte, bool) { stored++; });

	dat.poke(0x0100, 0x5A);
	dat.poke(0xE100, 0xA5);			// the fixed window
	dat.poke(0xFE00 + 0x08 + 1, 0x40);	// task 1, page 1 -> block $40
	CHECK(dat.peek(0x0100) == 0x5A);
	CHECK(dat.peek(0xE100) == 0xA5);
	CHECK(logged == 0 && stored == 0);	// none of it the guest's

	// through task 1, while task 0 runs
	dat.poke_task(1, 0x2005, 0x77);
	CHECK(dat.read_physical(((size_t)0x40 << 13) | 0x0005) == 0x77);
	CHECK(dat.peek_task(1, 0x2005) == 0x77);
	CHECK(dat.peek(0x2005) != 0x77);	// task 0's page 1 is elsewhere

	// an unmapped page drops the store, with no fault
	dat.poke(0xFE00 + 0x00, 0xFF);
	dat.poke(0x0010, 0x11);
	CHECK((bool)dat.NMI == true);
	dat.write(0x0010, 0x11);
	CHECK((bool)dat.NMI == false);		// the guest's store faults
}

static void test_watchpoint_poke()
{
	mc6809 cpu;
	SystemWatchpoint w(cpu);
	SystemWatchpointVec vec(w);
	w.ctrl_write(0x01);			// arm
	vec.poke(0x08, 0x99);
	CHECK(!w.nmi_asserted());
	CHECK(vec.peek(0x08) == 0x99);
	vec.write(0x08, 0x66);			// the guest's store trips it
	CHECK(w.nmi_asserted());
	CHECK(vec.peek(0x08) == 0x99);
}

// A debugger's memory read goes through peek()
static void test_debug_read()
{
	OneByte in;
	auto acia = std::make_shared<mc6850>(in, 1);
	auto ram = std::make_shared<RAM>(0x10000);
	mc6809 cpu;
	cpu.attach_range(acia, 0xFFC4, 0x0002);
	cpu.attach(ram, 0x0000, 0x0000);
	static_cast<ActiveDevice&>(*acia).tick(1);

	CHECK(cpu.debug_read(0xFFC5) == 'K');
	CHECK(cpu.debug_read(0xFFC4) & 0x01);	// still pending
	CHECK(cpu.read(0xFFC5) == 'K');
	CHECK(!(cpu.debug_read(0xFFC4) & 0x01));
}

int main()
{
	test_acia();
	test_tick();
	test_ide();
	test_dat();
	test_dat_poke();
	test_watchpoint_poke();
	test_debug_read();

	if (failures) {
		printf("test_peek: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_peek: PASS\n");
	return EXIT_SUCCESS;
}
