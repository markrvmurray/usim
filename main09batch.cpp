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
#include <vector>

#include "mc6809.h"
#include "mc6850.h"
#include "batchterm.h"
#include "haltdev.h"
#include "tracectl.h"
#include "memory.h"

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [--timeout=N] [--cycles] [--trace] [--watch=ADDR]"
		" [--brk=ADDR[,ADDR,...]] [--brk-gated] <hexfile>\n", prog);
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	unsigned long timeout = 0;
	bool trace = false;
	bool report_cycles = false;
	const char *hexfile = nullptr;

	Word watch_addr = 0;
	bool watching = false;
	std::vector<Word> brk_addrs;
	bool brk_gated = false;

	// Parse arguments
	for (int i = 1; i < argc; ++i) {
		if (strncmp(argv[i], "--timeout=", 10) == 0) {
			timeout = strtoul(argv[i] + 10, nullptr, 10);
		} else if (strcmp(argv[i], "--trace") == 0) {
			trace = true;
		} else if (strcmp(argv[i], "--cycles") == 0) {
			report_cycles = true;
		} else if (strncmp(argv[i], "--watch=", 8) == 0) {
			watch_addr = (Word)strtoul(argv[i] + 8, nullptr, 0);
			watching = true;
		} else if (strncmp(argv[i], "--brk=", 6) == 0) {
			char *p = argv[i] + 6;
			while (*p) {
				char *end;
				unsigned long a = strtoul(p, &end, 0);
				if (end == p) break;
				brk_addrs.push_back((Word)a);
				p = end;
				if (*p == ',') ++p;
			}
		} else if (strcmp(argv[i], "--brk-gated") == 0) {
			brk_gated = true;
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
	// BRK output is always-on unless --brk-gated is set, in which case
	// it starts disabled and is toggled by program-side writes to the
	// TraceCtl device (see tracectl.h). Lets a layout-sensitive test
	// trigger BRK output only around the suspect region without
	// otherwise disturbing codegen.
	bool brk_enabled = !brk_gated;

	auto ram = std::make_shared<RAM>(0x10000);
	auto rom = std::make_shared<ROM>(rom_size);
	auto acia = std::make_shared<mc6850>(term);
	auto halt = std::make_shared<HaltDevice>(cpu, &halted);
	auto tracectl = std::make_shared<TraceCtl>(cpu, &brk_enabled);

	// Attach order matters: first match wins in USim's device scan.
	// IO devices first (overlay 0xC000-0xC003), then ROM, then RAM.
	cpu.attach(acia, 0xc000, 0xfffe);
	cpu.attach(halt, 0xc002, 0xffff);
	cpu.attach(tracectl, 0xc003, 0xffff);
	cpu.attach(rom, rom_base, (Word)~(rom_size - 1));
	cpu.attach(ram, 0x0000, 0x0000);	// mask=0: matches all addresses (fallback)

	cpu.FIRQ.bind([&]() {
		return acia->IRQ;
	});

	// Load the same HEX file into both devices. Each silently drops
	// records outside its mapped range; together they cover all 64 K.
	// Required for picolibc no-flash binaries whose .text lives in RAM
	// at $0100 while .init (with the reset entry) lives in ROM at $C100.
	rom->load_intelhex(hexfile, rom_base);
	ram->load_intelhex(hexfile, 0x0000);

	cpu.reset();
	if (trace) cpu.tron();

	// Memory watchpoint: monitor writes to a specific address.
	// When the watched byte changes, print the new value and
	// instruction count. Use --trace alongside for full CPU state.
	Byte watch_prev = 0;
	if (watching) {
		watch_prev = cpu.read(watch_addr);
		fprintf(stderr, "WATCH: $%04X initial=%02X\n", watch_addr, watch_prev);
	}

	bool stepping = (timeout > 0) || !brk_addrs.empty();
	if (stepping) {
		unsigned long count = 0;
		while (!halted && (timeout == 0 || count < timeout)) {
			// Brk: dump full register state when PC is about to
			// execute one of the listed addresses. Print BEFORE
			// tick so register values reflect the inputs to the
			// upcoming instruction.
			if (!brk_addrs.empty() && brk_enabled) {
				Word cur_pc = cpu.get_pc();
				for (Word a : brk_addrs) {
					if (cur_pc == a) {
						fprintf(stderr, "BRK: PC=$%04X "
							"A=%02X B=%02X X=%04X Y=%04X "
							"U=%04X S=%04X CC=%02X "
							"[insn#%lu]\n",
							(unsigned)cur_pc,
							(unsigned)cpu.get_a(), (unsigned)cpu.get_b(),
							(unsigned)cpu.get_x(), (unsigned)cpu.get_y(),
							(unsigned)cpu.get_u(), (unsigned)cpu.get_s(),
							(unsigned)cpu.get_cc(),
							count);
						break;
					}
				}
			}
			cpu.tick();
			if (watching) {
				Byte cur = cpu.read(watch_addr);
				if (cur != watch_prev) {
					Byte next = cpu.read(watch_addr + 1);
					fprintf(stderr, "WATCH: $%04X %02X->%02X (word=%02X%02X) "
						"PC=$%04X A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X "
						"[insn#%lu]\n",
						watch_addr, watch_prev, cur, cur, next,
						(unsigned)cpu.get_insn_pc(),
						(unsigned)cpu.get_a(), (unsigned)cpu.get_b(),
						(unsigned)cpu.get_x(), (unsigned)cpu.get_y(),
						(unsigned)cpu.get_u(), (unsigned)cpu.get_s(),
						count);
					watch_prev = cur;
				}
			}
			++count;
		}
		if (!halted && timeout > 0) {
			if (report_cycles) {
				fprintf(stderr, "cycles=%llu\n",
					(unsigned long long)cpu.get_total_cycles());
			}
			fprintf(stderr, "usim09batch: timeout after %lu instructions\n", timeout);
			// rc 124 follows GNU `timeout(1)` convention so test
			// harnesses can distinguish a cap-hit from a real
			// non-zero exit set by the simulated program.
			return 124;
		}
	} else {
		cpu.run();
	}

	if (report_cycles) {
		fprintf(stderr, "cycles=%llu\n",
			(unsigned long long)cpu.get_total_cycles());
	}
	return halt->getExitCode();
}
