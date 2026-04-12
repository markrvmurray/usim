//
//	haltdev.h
//	Memory-mapped halt device: any write to this address halts the CPU
//
//	vim: ts=8
//

#pragma once

#include "device.h"
#include "usim.h"

class HaltDevice : public MappedDevice {

	USim&			cpu;
	bool*			flag;
	Byte			exit_code = 0;

public:
	virtual Byte		read(Word offset) {
					(void)offset;
					return 0xff;
				}

	virtual void		write(Word offset, Byte val) {
					(void)offset;
					exit_code = val;
					cpu.halt();
					if (flag) *flag = true;
				}

	Byte			getExitCode() const { return exit_code; }

				HaltDevice(USim& cpu, bool* flag = nullptr)
					: cpu(cpu), flag(flag) {}
	virtual			~HaltDevice() = default;

};
