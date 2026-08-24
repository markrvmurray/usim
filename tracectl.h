//
//	tracectl.h
//	Memory-mapped trace-control device.
//
//	Writes from the simulated program toggle host-side debug output
//	without altering CPU state — used to gate --brk and --trace output
//	to a precise window inside a long run, with minimal codegen impact
//	on the simulated program (one immediate store per poke).
//
//	Encoding:
//	  0x00  disable all
//	  0x01  enable --brk output
//	  0x02  enable --trace (and --brk)
//	  0x04  start counting cycles
//	  0x05  stop counting cycles
//	Other values are no-ops.
//
//	The counter accumulates the cycles between each 0x04 and the
//	0x05 that follows it, so a program can total the cost of one
//	region across many visits to it and leave everything else out.
//	It samples the CPU's own count at the two edges and so costs
//	nothing while it runs; what it does include is part of the
//	store that opens the window and part of the one that closes it,
//	a fixed overhead a null window measures exactly.
//
//	Instructions are counted the same way when the driver hands
//	over its instruction counter, which only exists when the run is
//	stepped.  Ticks say what the hardware costs; instructions say
//	whether a region is doing less work or merely reaching for
//	cheaper instructions, and unlike ticks they do not move when
//	the 6309 is put in native mode.
//
//	vim: ts=8
//

#pragma once

#include <cstdint>

#include "device.h"
#include "mc6809.h"

class TraceCtl : public MappedDevice {

	mc6809&			cpu;
	bool*			brk_enabled;
	uint64_t		gated = 0;
	uint64_t		windows = 0;
	uint64_t		opened_at = 0;
	bool			counting = false;
	const unsigned long*	insns = nullptr;
	uint64_t		gated_insns = 0;
	uint64_t		insns_at = 0;

public:
	uint64_t		gated_cycles() const { return gated; }
	uint64_t		gated_windows() const { return windows; }

	// Only a stepped run has an instruction count to offer; without
	// this the instruction half stays unreported rather than zero.
	bool			counts_instructions() const { return insns; }
	uint64_t		gated_instructions() const { return gated_insns; }
	void			count_instructions(const unsigned long *p)
				{ insns = p; }

	virtual Byte		read(Word offset) {
					(void)offset;
					return 0;
				}

	virtual void		write(Word offset, Byte val) {
					(void)offset;
					switch (val) {
					case 0x00:
						if (brk_enabled) *brk_enabled = false;
						cpu.troff();
						break;
					case 0x01:
						if (brk_enabled) *brk_enabled = true;
						break;
					case 0x02:
						if (brk_enabled) *brk_enabled = true;
						cpu.tron();
						break;
					case 0x04:
						opened_at = cpu.get_total_cycles();
						if (insns) insns_at = *insns;
						counting = true;
						break;
					case 0x05:
						if (counting) {
							gated += cpu.get_total_cycles()
								- opened_at;
							if (insns)
								gated_insns +=
									*insns - insns_at;
							++windows;
							counting = false;
						}
						break;
					default:
						break;
					}
				}

				TraceCtl(mc6809& cpu, bool* brk_enabled = nullptr)
					: cpu(cpu), brk_enabled(brk_enabled) {}
	virtual			~TraceCtl() = default;

};
