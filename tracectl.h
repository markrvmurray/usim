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
//	Other values are no-ops.
//
//	vim: ts=8
//

#pragma once

#include "device.h"
#include "mc6809.h"

class TraceCtl : public MappedDevice {

	mc6809&			cpu;
	bool*			brk_enabled;

public:
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
					default:
						break;
					}
				}

				TraceCtl(mc6809& cpu, bool* brk_enabled = nullptr)
					: cpu(cpu), brk_enabled(brk_enabled) {}
	virtual			~TraceCtl() = default;

};
