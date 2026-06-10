//
//	mc6850.h
//	(C) R.P.Bellis 1994 - 2025
//
//	vim: ts=8
//

#pragma once

#include "device.h"
#include "wiring.h"

class mc6850_impl {

public:
	virtual bool		poll_read() = 0;
	virtual bool		poll_write() { return true; };

public:
	virtual Byte		read() = 0;
	virtual void		write(Byte) = 0;

public:
				mc6850_impl() = default;
	virtual			~mc6850_impl() = default;
};

class mc6850 : virtual public ActiveMappedDevice {

protected:
	enum sr_flags : Byte {
		RDRF	= 0x01,
		TDRE	= 0x02,
		DCDB	= 0x04,
		CTSB	= 0x08,
		FE	= 0x10,
		OVRN	= 0x20,
		PE	= 0x40,
		IRQB	= 0x80
	};

// Internal registers
protected:

	Byte			td, rd, cr, sr;
	// RTS flow control (U-080). The guest deasserts RTS (CR6:CR5 = 10) to say
	// "stop sending"; while deasserted we do NOT consume host input, so bytes
	// stay buffered in the host pipe (no console RX overrun). On a real serial
	// line RTS is a wire; here the console is stdin/stdout, so "honour RTS" =
	// "pause reading stdin" -- the OS pipe (~64K on macOS) holds the held bytes
	// and back-pressures the feeder, which is exactly the behaviour we want.
	bool			rts = true;	// true = asserted (host may send)

// Access to real IO device
	mc6850_impl&		impl;
	uint16_t		interval;	// how often to poll
	uint16_t		cycles;		// cycles since last poll

// Initialisation functions

protected:
	virtual void		tick(uint8_t);
	virtual void		reset();

// Read and write functions
public:

	virtual Byte		read(Word offset);
	virtual void		write(Word offset, Byte val);

// Other exposed interfaces
public:
	OutputPinReg		IRQ;

// Public constructor and destructor

				mc6850(mc6850_impl& impl, uint16_t interval = 1000);
	virtual			~mc6850();

};
