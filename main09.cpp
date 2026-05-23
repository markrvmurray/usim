//
//	main09.cpp
//	(C) R.P.Bellis 1993 - 2025
//
//	vim: ts=8 sw=8 noet:
//
//	Example of a simple MC6809 system with:
//	  $0000-$7FFF  32 KB RAM
//	  $C000-$C001  MC6850 ACIA (status/data)
//	  $E000-$FFFF  8 KB ROM
//	  $FFCA        SystemWatchpoint control
//	  $FFE0-$FFFF  SystemWatchpoint vector + snippet shadow (overlays ROM)
//
//	The SystemWatchpoint attaches before the ROM so it wins for its
//	addresses; the ROM still backs everything else in $E000-$FFFF.
//

#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>

#include "mc6809.h"
#include "mc6850.h"
#include "term.h"
#include "memory.h"
#include "system_watchpoint.h"

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: usim09 <hexfile>\n");
		return EXIT_FAILURE;
	}

	(void)signal(SIGINT, SIG_IGN);

	const Word ram_size = 0x8000;
	const Word rom_base = 0xe000;
	const Word rom_size = 0x10000 - rom_base;

	mc6809			cpu;
	Terminal 		term(cpu);

	auto ram        = std::make_shared<RAM>(ram_size);
	auto rom        = std::make_shared<ROM>(rom_size);
	auto acia       = std::make_shared<mc6850>(term);
	auto watch      = std::make_shared<SystemWatchpoint>(cpu);
	auto watch_ctrl = std::make_shared<SystemWatchpointCtrl>(*watch);
	auto watch_vec  = std::make_shared<SystemWatchpointVec>(*watch);

	// Attach the SystemWatchpoint ahead of the ROM so it wins for
	// $FFCA and $FFE0-$FFFF. Reads from those addresses go through
	// the watchpoint (seeded from ROM at startup); writes are blocked
	// when armed and asserted to NMI. The ROM still backs everything
	// else in $E000-$FFFF.
	cpu.attach(watch);					// ActiveDevice: reset disarms
	cpu.attach_range(watch_ctrl, 0xffca, 0x0001);		// $FFCA
	cpu.attach_range(watch_vec,  0xffe0, 0x0020);		// $FFE0-$FFFF
	cpu.attach(ram,  0x0000, ~(ram_size - 1));
	cpu.attach(rom,  rom_base, ~(rom_size - 1));
	cpu.attach(acia, 0xc000, 0xfffe);

	cpu.FIRQ.bind([&]() {
		return acia->IRQ;
	});
	cpu.NMI.bind([&]() {
		return (bool)watch->NMI;
	});

	rom->load_intelhex(argv[1], rom_base);

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
