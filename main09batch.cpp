//
//	main09batch.cpp
//	Batch-mode MC6809 system for automated testing
//
//	vim: ts=8 sw=8 noet:
//
//	Memory map:
//	  0x0000-0xBFFF  RAM (48 KB)
//	  0xC000-0xC002  IO devices (overlay ROM):
//	    0xC000-0xC001  ACIA (status/data)
//	    0xC002         Halt/exit device (written value = exit code)
//	  0xC003-0xFFFF  ROM (code + vectors)
//
//	BatchTerminal (unbuffered stdio, no termios)
//	--timeout=N instruction count limit
//

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "mc6809.h"
#include "mc6850.h"
#include "batchterm.h"
#include "haltdev.h"
#include "memory.h"

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [--timeout=N] <hexfile>\n", prog);
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	unsigned long timeout = 0;
	bool trace = false;
	const char *hexfile = nullptr;

	// Parse arguments
	for (int i = 1; i < argc; ++i) {
		if (strncmp(argv[i], "--timeout=", 10) == 0) {
			timeout = strtoul(argv[i] + 10, nullptr, 10);
		} else if (strcmp(argv[i], "--trace") == 0) {
			trace = true;
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
		} else {
			if (hexfile) usage(argv[0]);
			hexfile = argv[i];
		}
	}

	if (!hexfile) {
		usage(argv[0]);
	}

	const Word rom_base = 0xc000;
	const Word rom_size = 0x10000 - rom_base;

	bool			halted = false;
	mc6809			cpu;
	BatchTerminal		term;

	// 64 KB RAM as fallback (covers 0x0000-0xFFFF).
	// ROM and IO devices are attached first so they take priority
	// in the 0xC000+ range. RAM is only reached for 0x0000-0xBFFF
	// (48 KB effective).
	auto ram = std::make_shared<RAM>(0x10000);
	auto rom = std::make_shared<ROM>(rom_size);
	auto acia = std::make_shared<mc6850>(term);
	auto halt = std::make_shared<HaltDevice>(cpu, &halted);

	// Attach order matters: first match wins in USim's device scan.
	// IO devices first (overlay 0xC000-0xC002), then ROM, then RAM.
	cpu.attach(acia, 0xc000, 0xfffe);
	cpu.attach(halt, 0xc002, 0xffff);
	cpu.attach(rom, rom_base, (Word)~(rom_size - 1));
	cpu.attach(ram, 0x0000, 0x0000);	// mask=0: matches all addresses (fallback)

	cpu.FIRQ.bind([&]() {
		return acia->IRQ;
	});

	rom->load_intelhex(hexfile, rom_base);

	cpu.reset();
	if (trace) cpu.tron();

	if (timeout > 0) {
		unsigned long count = 0;
		while (!halted && count < timeout) {
			cpu.tick();
			++count;
		}
		if (!halted) {
			fprintf(stderr, "usim09batch: timeout after %lu instructions\n", timeout);
			return EXIT_FAILURE;
		}
	} else {
		cpu.run();
	}

	return halt->getExitCode();
}
