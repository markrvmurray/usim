//
//	picotick.h
//	Pico-Thing 50Hz Tick Timer
//
//	Two registers at $FFC8-$FFC9:
//	  $FFC8 (ctrl):   bit 0 = enable (write-only)
//	  $FFC9 (status): bit 7 = IRQ pending (read clears)
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include "device.h"
#include "wiring.h"

class PicoTick : virtual public ActiveMappedDevice {

protected:
	Byte		ctrl;
	Byte		status;
	uint32_t	counter;
	uint32_t	threshold;

public:
	OutputPinReg	IRQ;

			PicoTick(uint32_t cycles_per_tick = 20000);
	virtual		~PicoTick() = default;

	void		reset() override;
	void		tick(uint8_t cycles) override;
	Byte		read(Word offset) override;
	void		write(Word offset, Byte val) override;
};
