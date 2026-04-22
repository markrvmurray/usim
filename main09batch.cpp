//
//	main09batch.cpp
//	Batch-mode MC6809 system for automated testing
//
//	vim: ts=8 sw=8 noet:
//
//	Memory map (top-to-bottom; $0000 is at the top, $FFFF at the
//	bottom):
//	  0x0000-0xFFCF  RAM (continuous, ~64 KB)
//	  0xFFD0-0xFFEF  IO slab (just above the vectors):
//	    0xFFD0-0xFFD1  ACIA (status/data)
//	    0xFFD2         Halt/exit device (written value = exit code)
//	    0xFFD3         TraceCtl (--brk-gated toggle, --trace toggle)
//	    0xFFD4-0xFFEF  reserved for future IO
//	  0xFFF0-0xFFFF  vector ROM (16 bytes; reset vector at $FFFE
//	                 injected into the hex file by the run-mc6809
//	                 wrapper, loaded at startup, write-protected
//	                 thereafter so a runaway program can't overwrite
//	                 the reset entry)
//
//	The previous large $C100-$FFFF ROM region and the $C000-region
//	IO overlay are gone — there is no longer a real ROM/RAM
//	distinction in the emulated machine. A single unified RAM with
//	the IO devices and a tiny vector ROM parked at the bottom
//	gives picolibc ~64 KB of contiguous address space without an
//	IO wedge in the middle, while still preserving the "vectors
//	can't be clobbered" semantic of a real machine.
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

	bool			halted = false;
	mc6809			cpu;
	BatchTerminal		term;

	// BRK output is always-on unless --brk-gated is set, in which case
	// it starts disabled and is toggled by program-side writes to the
	// TraceCtl device (see tracectl.h). Lets a layout-sensitive test
	// trigger BRK output only around the suspect region without
	// otherwise disturbing codegen.
	bool brk_enabled = !brk_gated;

	auto ram = std::make_shared<RAM>(0x10000);
	auto vrom = std::make_shared<ROM>(0x10);    // 16-byte vector ROM at $FFF0-$FFFF
	auto acia = std::make_shared<mc6850>(term);
	auto halt = std::make_shared<HaltDevice>(cpu, &halted);
	auto tracectl = std::make_shared<TraceCtl>(cpu, &brk_enabled);

	// Attach order matters: first match wins in USim's device scan.
	// Vector ROM and IO devices first, RAM as fallback.
	//   $FFF0-$FFFF  vector ROM (mask 0xFFF0 catches all 16 bytes)
	//   $FFD0-$FFD1  ACIA (status, data)
	//   $FFD2        Halt
	//   $FFD3        TraceCtl
	// RAM covers everything that hasn't been claimed above.
	cpu.attach(vrom,     0xfff0, 0xfff0); // vector ROM: $FFF0-$FFFF
	cpu.attach(acia,     0xffd0, 0xfffe); // ACIA: $FFD0 (status), $FFD1 (data)
	cpu.attach(halt,     0xffd2, 0xffff); // Halt: $FFD2
	cpu.attach(tracectl, 0xffd3, 0xffff); // TraceCtl: $FFD3
	cpu.attach(ram,      0x0000, 0x0000); // mask=0: matches all addresses (fallback)

	cpu.FIRQ.bind([&]() {
		return acia->IRQ;
	});

	// Load the HEX file into both the RAM and the vector ROM.
	// Each silently drops records outside its mapped range, so the
	// vector ROM picks up only the $FFF0-$FFFF records (the reset
	// vector at $FFFE injected by the run-mc6809 wrapper) and RAM
	// picks up everything else. After this, the vector ROM is
	// write-protected — programs can't clobber the reset entry.
	vrom->load_intelhex(hexfile, 0xfff0);
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
