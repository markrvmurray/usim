//
//	main09.cpp
//	(C) R.P.Bellis 1993 - 2025
//
//	vim: ts=8 sw=8 noet:
//
//	Example of a simple MC6809 system with:
//	  $0000-$7FFF  32 KB RAM
//	  $E000-$FFFF  8 KB ROM
//	  $FFC4-$FFC5  ACIA (status/data) — pico-thing console ACIA slot
//	  $FFCA        SystemWatchpoint control
//	  $FFE0-$FFFF  SystemWatchpoint vector + snippet shadow (overlays ROM)
//
//	ACIA and SystemWatchpoint attach before ROM so they win for their
//	addresses; the ROM still backs everything else in $E000-$FFFF.
//

#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>

#include <cstring>
#include <memory>

#include "mc6809.h"
#include "hd6309.h"
#include "mc6850.h"
#include "term.h"
#include "memory.h"
#include "system_watchpoint.h"

int main(int argc, char *argv[])
{
	bool use_hd6309 = false;
	const char *hexfile = nullptr;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--hd6309") == 0) {
			use_hd6309 = true;
		} else if (argv[i][0] != '-' && !hexfile) {
			hexfile = argv[i];
		} else {
			hexfile = nullptr;
			break;
		}
	}
	if (!hexfile) {
		fprintf(stderr, "usage: usim09 [--hd6309] <hexfile>\n");
		return EXIT_FAILURE;
	}

	(void)signal(SIGINT, SIG_IGN);

	const Word ram_size = 0x8000;
	const Word rom_base = 0xe000;
	const Word rom_size = 0x10000 - rom_base;

	std::unique_ptr<mc6809>	cpup(use_hd6309
				     ? static_cast<mc6809*>(new hd6309())
				     : new mc6809());
	mc6809&			cpu = *cpup;
	Terminal 		term(cpu);

	auto ram        = std::make_shared<RAM>(ram_size);
	auto rom        = std::make_shared<ROM>(rom_size);
	auto acia       = std::make_shared<mc6850>(term);
	auto watch      = std::make_shared<SystemWatchpoint>(cpu);
	auto watch_ctrl = std::make_shared<SystemWatchpointCtrl>(*watch);
	auto watch_vec  = std::make_shared<SystemWatchpointVec>(*watch);

	// Attach ACIA + SystemWatchpoint ahead of the ROM so they win
	// for their addresses. ACIA at $FFC4-$FFC5 matches pico-thing's
	// console ACIA slot so a debug-build firmware can target both
	// drivers with the same MMIO addresses.
	cpu.attach(watch);					// ActiveDevice: reset disarms
	cpu.attach_range(acia,       0xffc4, 0x0002);		// $FFC4-$FFC5
	cpu.attach_range(watch_ctrl, 0xffca, 0x0001);		// $FFCA
	cpu.attach_range(watch_vec,  0xffe0, 0x0020);		// $FFE0-$FFFF
	cpu.attach(ram,  0x0000, ~(ram_size - 1));
	cpu.attach(rom,  rom_base, ~(rom_size - 1));

	cpu.IRQ.bind([&]() {
		return acia->IRQ;
	});
	cpu.NMI.bind([&]() {
		return (bool)watch->NMI;
	});

	// Load firmware (format sniffed from magic):
	//   $7F  → ELF32 (llvm-mc6809 / lld output)
	//   'S'  → Motorola S-record
	//   ':'  → Intel HEX (the historical default)
	{
		FILE *probe = fopen(hexfile, "rb");
		if (!probe) { perror(hexfile); return EXIT_FAILURE; }
		unsigned char magic[4] = {0};
		(void)fread(magic, 1, 4, probe);
		fclose(probe);
		if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
			rom->load_elf(hexfile, rom_base);
		} else if (magic[0] == 'S') {
			rom->load_srec(hexfile, rom_base);
		} else {
			rom->load_intelhex(hexfile, rom_base);
		}
	}

	// Seed the watchpoint shadow from the ROM image so reads to
	// $FFE0-$FFFF (including the reset vector at $FFFE) reach the
	// CPU through the watchpoint.
	for (Word a = 0xFFE0; a != 0; a++) {
		watch->load(a, rom->read(a - rom_base));
		if (a == 0xFFFF) break;
	}

	cpu.reset();
	cpu.run();

	return EXIT_SUCCESS;
}
