//
//	mos6522.h
//	6522 Versatile Interface Adapter
//
//	Sixteen registers, selected by RS3-RS0:
//	  $0 ORB/IRB   $4 T1C-L   $8 T2C-L   $C PCR
//	  $1 ORA/IRA   $5 T1C-H   $9 T2C-H   $D IFR
//	  $2 DDRB      $6 T1L-L   $A SR      $E IER
//	  $3 DDRA      $7 T1L-H   $B ACR     $F ORA/IRA, no handshake
//
//	Everything counts in phi2 cycles, one per CPU cycle, stepped one
//	cycle at a time by tick(). Register side effects -- which access
//	clears which flag, what starts the handshakes and the shifter --
//	follow MAME's 6522via.cpp; timing follows the datasheet:
//	  T1 one-shot   IFR6 is set N+1 cycles after T1C-H is written
//	  T1 free-run   IFR6 is set every N+2 cycles, PB7 toggling with it
//	  T2 one-shot   IFR5 is set N+1 cycles after T2C-H is written,
//	                once per write
//	  T2 pulses     counts PB6 falling edges, IFR5 on underflow
//	  SR            8 bits under T2 (N+2 cycles per CB1 edge), phi2
//	                (a bit per cycle) or an external CB1, in or out;
//	                mode 4 recirculates the byte forever without IFR2
//	  CA2/CB2       input edge, independent input, handshake, pulse
//	                (one cycle low) and manual output modes
//	  PA/PB         input latching on the CA1/CB1 active edge
//
//	The device only learns of a register access after it happens and
//	is ticked for the whole instruction afterwards, so a timer read
//	back straight after it was loaded can be a few cycles further on
//	than the chip would be. Nothing that counts intervals is affected.
//
//	The far side of the pins is driven through set_pa(), set_ca1()
//	and the rest; an input nothing drives floats high, as the NMOS
//	chip's pull-ups make it. The pin accessors report what the pins
//	carry, outputs and inputs alike, for anything that displays them.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include "device.h"
#include "wiring.h"

class mos6522 : virtual public ActiveMappedDevice {

public:
	enum reg : Word {
		ORB = 0, ORA, DDRB, DDRA, T1CL, T1CH, T1LL, T1LH,
		T2CL, T2CH, SR, ACR, PCR, IFR, IER, ORA_NH
	};

	enum ifr_flags : Byte {
		IF_CA2	= 0x01,
		IF_CA1	= 0x02,
		IF_SR	= 0x04,
		IF_CB2	= 0x08,
		IF_CB1	= 0x10,
		IF_T2	= 0x20,
		IF_T1	= 0x40,
		IF_ANY	= 0x80
	};

protected:
	// registers
	Byte			ora, orb, ddra, ddrb;
	Byte			ira_latch, irb_latch;
	Byte			t1ll, t1lh, t2ll;
	Word			t1c, t2c;
	Byte			sr, acr, pcr, ifr, ier;

	// timer state
	bool			t1_armed;	// one-shot still to fire
	bool			t1_reload;	// free-run: reload from the latch this cycle
	bool			t2_armed;
	bool			pb7;		// T1's PB7 output (ACR7)

	// shifter state
	bool			sr_running;
	int			sr_bits;	// bits still to shift
	uint32_t		sr_wait;	// cycles to the next CB1 edge

	// handshake pulse outputs: cycles until they return high
	int			ca2_pulse, cb2_pulse;

	// the pins: what the outside drives, and what the chip drives
	Byte			in_a, in_b;
	bool			in_ca1, in_ca2, in_cb1, in_cb2;
	bool			out_ca2, out_cb1, out_cb2;

	void			set_int(Byte flags);
	void			clear_int(Byte flags);
	void			update_irq();

	int			sr_mode() const { return (acr >> 2) & 7; }
	bool			sr_internal_clock() const;
	uint32_t		sr_period() const;
	void			sr_start();
	void			sr_edge(bool cb1);

	void			ca2_handshake();
	void			cb2_handshake();
	Byte			read_ira() const;
	Byte			read_irb() const;

	void			step();

public:
	OutputPinReg		IRQ;		// ~IRQ = IFR bit 7

				mos6522();
	virtual			~mos6522() = default;

	void			reset() override;
	void			tick(uint32_t cycles) override;
	Byte			read(Word offset) override;
	void			write(Word offset, Byte val) override;
	Byte			peek(Word offset) override;

	// the far side of the pins
	void			set_pa(Byte val);
	void			set_pb(Byte val);
	void			set_ca1(bool level);
	void			set_ca2(bool level);
	void			set_cb1(bool level);
	void			set_cb2(bool level);

	// what the pins carry, and which of them the chip is driving
	Byte			pa() const;
	Byte			pb() const;
	Byte			pa_driven() const { return ddra; }
	Byte			pb_driven() const;
	bool			ca1() const { return in_ca1; }
	bool			ca2() const;
	bool			ca2_driven() const;
	bool			cb1() const;
	bool			cb1_driven() const;
	bool			cb2() const;
	bool			cb2_driven() const;
	bool			irq() const { return (ifr & IF_ANY) != 0; }
};
