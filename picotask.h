//
//	picotask.h
//	Pico-Thing DAT Task Register
//
//	Write-only register at $FFC0 that selects the active task (0-31)
//	for DAT address translation.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include "device.h"
#include "memory.h"

class PicoTask : public MappedDevice {

	DATRAM&		datram;

public:
			PicoTask(DATRAM& datram) : datram(datram) {}

	Byte		read(Word offset) override {
				(void)offset;
				return 0xFF;	// write-only
			}

	void		write(Word offset, Byte val) override {
				(void)offset;
				datram.set_task(val);
			}
};
