//
//	picotask.h
//	Pico-Thing DAT Task Register
//
//	Register at $FFC0 that selects the active task (0-31) for DAT
//	address translation.  Reads return the current task number: the
//	Pico firmware mirrors every accepted task write into the read
//	register (pico_thing.cpp services SYSTEM_TASK writes with
//	reg[SYSTEM_TASK] = task_change(wval)), so the guest can read back
//	which task is active.  NitrOS-9's SWICall relies on this to tell
//	a system-state caller (task 0) from a user-state caller.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include "device.h"
#include "memory.h"

class PicoTask : public MappedDevice {

	DATRAM&		datram;
	Byte		task = 0;

public:
			PicoTask(DATRAM& datram) : datram(datram) {}

	Byte		read(Word offset) override {
				(void)offset;
				return task;	// firmware mirrors task into the read register
			}

	void		write(Word offset, Byte val) override {
				(void)offset;
				task = (Byte)(val & 0x1F);	// 5-bit task number
				datram.set_task(val);
			}
};
