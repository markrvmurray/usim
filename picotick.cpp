//
//	picotick.cpp
//	Pico-Thing 50Hz Tick Timer
//
//	vim: ts=8 sw=8 noet:
//

#include "picotick.h"

PicoTick::PicoTick(uint32_t cycles_per_tick)
	: ctrl(0), status(0), counter(0), threshold(cycles_per_tick),
	  IRQ(status, 7, true)	// inverted: active-low (0 = asserted)
{
}

void PicoTick::reset()
{
	ctrl = 0;
	status = 0;
	counter = 0;
}

void PicoTick::tick(uint8_t cycles)
{
	if (!(ctrl & 0x01))
		return;		// timer disabled

	counter += cycles;
	if (counter >= threshold) {
		counter -= threshold;
		status |= 0x80;	// set IRQ pending
	}
}

Byte PicoTick::read(Word offset)
{
	switch (offset) {
	case 0:		// ctrl register
		return ctrl;
	case 1:		// status register - reading clears IRQ
	{
		Byte s = status;
		status &= ~0x80;
		return s;
	}
	default:
		return 0xFF;
	}
}

void PicoTick::write(Word offset, Byte val)
{
	switch (offset) {
	case 0:		// ctrl register
		ctrl = val;
		if (!(ctrl & 0x01)) {
			status &= ~0x80;	// disable clears pending IRQ
			counter = 0;
		}
		break;
	case 1:		// status is read-only
		break;
	}
}
