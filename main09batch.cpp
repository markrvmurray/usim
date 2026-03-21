//
//	main09batch.cpp
//	Batch-mode MC6809 system for automated testing
//
//	vim: ts=8 sw=8 noet:
//
//	Same memory map as main09.cpp but with:
//	- BatchTerminal (unbuffered stdio, no termios)
//	- HaltDevice at 0xbf00 (write to halt CPU)
//	- --timeout=N instruction count limit
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
	const char *hexfile = nullptr;

	// Parse arguments
	for (int i = 1; i < argc; ++i) {
		if (strncmp(argv[i], "--timeout=", 10) == 0) {
			timeout = strtoul(argv[i] + 10, nullptr, 10);
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

	const Word ram_size = 0x8000;
	const Word rom_base = 0xe000;
	const Word rom_size = 0x10000 - rom_base;

	bool			halted = false;
	mc6809			cpu;
	BatchTerminal		term;

	auto ram = std::make_shared<RAM>(ram_size);
	auto rom = std::make_shared<ROM>(rom_size);
	auto acia = std::make_shared<mc6850>(term);
	auto halt = std::make_shared<HaltDevice>(cpu, &halted);

	cpu.attach(ram, 0x0000, ~(ram_size - 1));
	cpu.attach(rom, rom_base, ~(rom_size - 1));
	cpu.attach(acia, 0xc000, 0xfffe);
	cpu.attach(halt, 0xbf00, 0xffff);

	cpu.FIRQ.bind([&]() {
		return acia->IRQ;
	});

	rom->load_intelhex(hexfile, rom_base);

	cpu.reset();

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

	return EXIT_SUCCESS;
}
