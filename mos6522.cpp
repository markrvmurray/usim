//
//	mos6522.cpp
//	6522 Versatile Interface Adapter (see mos6522.h)
//
//	vim: ts=8 sw=8 noet:
//

#include "mos6522.h"

mos6522::mos6522()
	: t1ll(0xff), t1lh(0xff), t2ll(0xff),
	  t1c(0xffff), t2c(0xffff), sr(0),
	  in_a(0xff), in_b(0xff),
	  in_ca1(true), in_ca2(true), in_cb1(true), in_cb2(true),
	  IRQ(ifr, 7, true)	// inverted: active-low (0 = asserted)
{
	reset();
}

// RES clears every register but the timers and the shifter, and stops
// them interrupting; the counters themselves carry on decrementing.
void mos6522::reset()
{
	ora = orb = ddra = ddrb = 0;
	ira_latch = irb_latch = 0;
	acr = pcr = ifr = ier = 0;

	t1_armed = t1_reload = t2_armed = false;
	pb7 = true;

	sr_running = false;
	sr_bits = 0;
	sr_wait = 1;

	ca2_pulse = cb2_pulse = 0;
	out_ca2 = out_cb1 = out_cb2 = true;
}

//----------------------------------------------------------------------------
// Interrupts
//----------------------------------------------------------------------------

void mos6522::update_irq()
{
	if (ifr & ier & 0x7f)
		ifr |= IF_ANY;
	else
		ifr &= ~IF_ANY;
}

void mos6522::set_int(Byte flags)
{
	ifr |= flags;
	update_irq();
}

void mos6522::clear_int(Byte flags)
{
	ifr &= ~flags;
	update_irq();
}

//----------------------------------------------------------------------------
// The clock
//----------------------------------------------------------------------------

void mos6522::tick(uint32_t cycles)
{
	while (cycles-- > 0)
		step();
}

void mos6522::step()
{
	// a pulse-mode CA2/CB2 stays low for one cycle
	if (ca2_pulse > 0 && --ca2_pulse == 0)
		out_ca2 = true;
	if (cb2_pulse > 0 && --cb2_pulse == 0)
		out_cb2 = true;

	// T1 counts down through zero to $FFFF, where it times out. Free-
	// running, it reloads from the latch on the cycle after, so the
	// period is N+2; one-shot, it carries on down and stays quiet.
	if (t1_reload) {
		t1_reload = false;
		t1c = t1ll | (t1lh << 8);
	} else if (t1c-- == 0) {
		if (acr & 0x40) {
			pb7 = !pb7;
			t1_reload = true;
			set_int(IF_T1);
		} else if (t1_armed) {
			t1_armed = false;
			pb7 = true;
			set_int(IF_T1);
		}
	}

	// T2 is one-shot only; in pulse-counting mode PB6 clocks it instead
	if ((acr & 0x20) == 0 && t2c-- == 0 && t2_armed) {
		t2_armed = false;
		set_int(IF_T2);
	}

	// the shifter's own clock: under phi2 a whole bit goes by every
	// cycle, under T2 one CB1 edge every N+2 cycles
	if (sr_running && sr_internal_clock()) {
		if (sr_mode() == 2 || sr_mode() == 6) {
			sr_edge(false);
			sr_edge(true);
		} else if (--sr_wait == 0) {
			sr_wait = sr_period();
			sr_edge(!out_cb1);
		}
	}
}

//----------------------------------------------------------------------------
// The shift register
//----------------------------------------------------------------------------

// modes 1, 2, 4, 5 and 6 clock themselves, driving CB1
bool mos6522::sr_internal_clock() const
{
	const int mode = sr_mode();
	return mode != 0 && mode != 3 && mode != 7;
}

uint32_t mos6522::sr_period() const
{
	return t2ll + 2;
}

// reading or writing SR starts eight bits
void mos6522::sr_start()
{
	clear_int(IF_SR);
	sr_bits = 8;
	sr_running = sr_mode() != 0;
	sr_wait = (sr_mode() == 2 || sr_mode() == 6) ? 1 : sr_period();
	if (sr_internal_clock())
		out_cb1 = true;
}

// One CB1 edge, at the new level. Output modes put the top bit on CB2
// as CB1 falls, recirculating it into the bottom; input modes take
// CB2 in as CB1 rises. The eighth rising edge finishes the byte.
void mos6522::sr_edge(bool cb1)
{
	const int mode = sr_mode();

	if (sr_internal_clock())
		out_cb1 = cb1;

	if (!cb1) {
		if (mode >= 4) {
			out_cb2 = (sr & 0x80) != 0;
			sr = (sr << 1) | (out_cb2 ? 1 : 0);
		}
		return;
	}

	if (mode <= 3)
		sr = (sr << 1) | (in_cb2 ? 1 : 0);
	if (--sr_bits > 0)
		return;
	if (mode == 4) {
		sr_bits = 8;	// free-running: round again, no interrupt
		return;
	}
	sr_running = false;
	set_int(IF_SR);
}

//----------------------------------------------------------------------------
// The ports
//----------------------------------------------------------------------------

Byte mos6522::pa() const
{
	return (ora & ddra) | (in_a & ~ddra);
}

// output bits read back from ORB, not from the pin
Byte mos6522::pb() const
{
	Byte val = (orb & ddrb) | (in_b & ~ddrb);

	if (acr & 0x80)
		val = (val & 0x7f) | (pb7 ? 0x80 : 0);
	return val;
}

Byte mos6522::pb_driven() const
{
	return ddrb | (acr & 0x80);
}

Byte mos6522::read_ira() const
{
	return (acr & 0x01) ? ira_latch : pa();
}

Byte mos6522::read_irb() const
{
	return (acr & 0x02) ? irb_latch : pb();
}

bool mos6522::ca2_driven() const
{
	return (pcr & 0x08) != 0;
}

bool mos6522::ca2() const
{
	return ca2_driven() ? out_ca2 : in_ca2;
}

bool mos6522::cb1_driven() const
{
	return sr_internal_clock();
}

bool mos6522::cb1() const
{
	return cb1_driven() ? out_cb1 : in_cb1;
}

// shifting out takes CB2 over, whatever PCR says
bool mos6522::cb2_driven() const
{
	return (pcr & 0x80) != 0 || sr_mode() >= 4;
}

bool mos6522::cb2() const
{
	return cb2_driven() ? out_cb2 : in_cb2;
}

// an access to ORA drops CA2 in the handshake and pulse modes
void mos6522::ca2_handshake()
{
	switch (pcr & 0x0e) {
	case 0x08:	// handshake: low until the next CA1 active edge
		out_ca2 = false;
		break;
	case 0x0a:	// pulse: low for a cycle
		out_ca2 = false;
		ca2_pulse = 1;
		break;
	}
}

// and a write to ORB does the same for CB2
void mos6522::cb2_handshake()
{
	if (sr_mode() >= 4)
		return;
	switch (pcr & 0xe0) {
	case 0x80:
		out_cb2 = false;
		break;
	case 0xa0:
		out_cb2 = false;
		cb2_pulse = 1;
		break;
	}
}

// the flags a port access clears: CA2/CB2's only when it is not an
// independent interrupt input
static Byte ca_flags(Byte pcr)
{
	return mos6522::IF_CA1 | (((pcr & 0x0a) == 0x02) ? 0 : mos6522::IF_CA2);
}

static Byte cb_flags(Byte pcr)
{
	return mos6522::IF_CB1 | (((pcr & 0xa0) == 0x20) ? 0 : mos6522::IF_CB2);
}

//----------------------------------------------------------------------------
// The far side of the pins
//----------------------------------------------------------------------------

void mos6522::set_pa(Byte val)
{
	in_a = val;
}

void mos6522::set_pb(Byte val)
{
	const bool fell = (in_b & 0x40) && !(val & 0x40);

	in_b = val;

	// pulse counting: PB6 falling edges clock T2, which interrupts
	// once on underflow per write of T2C-H
	if (fell && (acr & 0x20) && !(ddrb & 0x40) && t2c-- == 0 && t2_armed) {
		t2_armed = false;
		set_int(IF_T2);
	}
}

void mos6522::set_ca1(bool level)
{
	if (level == in_ca1)
		return;
	in_ca1 = level;
	if (level != ((pcr & 0x01) != 0))
		return;

	if (acr & 0x01)
		ira_latch = pa();
	set_int(IF_CA1);
	if ((pcr & 0x0e) == 0x08)
		out_ca2 = true;		// handshake complete
}

void mos6522::set_ca2(bool level)
{
	if (level == in_ca2)
		return;
	in_ca2 = level;
	if (!ca2_driven() && level == ((pcr & 0x04) != 0))
		set_int(IF_CA2);
}

void mos6522::set_cb1(bool level)
{
	if (level == in_cb1)
		return;
	in_cb1 = level;
	if (cb1_driven())
		return;

	if (level == ((pcr & 0x10) != 0)) {
		if (acr & 0x02)
			irb_latch = pb();
		set_int(IF_CB1);
		if ((pcr & 0xe0) == 0x80 && sr_mode() < 4)
			out_cb2 = true;
	}

	// modes 3 and 7 shift on CB1 from outside
	if (sr_running)
		sr_edge(level);
}

void mos6522::set_cb2(bool level)
{
	if (level == in_cb2)
		return;
	in_cb2 = level;
	if (!cb2_driven() && level == ((pcr & 0x40) != 0))
		set_int(IF_CB2);
}

//----------------------------------------------------------------------------
// The CPU side
//----------------------------------------------------------------------------

Byte mos6522::read(Word offset)
{
	Byte val;

	switch (offset & 0x0f) {
	case ORB:
		val = read_irb();
		clear_int(cb_flags(pcr));
		return val;
	case ORA:
		val = read_ira();
		clear_int(ca_flags(pcr));
		ca2_handshake();
		return val;
	case DDRB:
		return ddrb;
	case DDRA:
		return ddra;
	case T1CL:
		clear_int(IF_T1);
		return t1c & 0xff;
	case T1CH:
		return t1c >> 8;
	case T1LL:
		return t1ll;
	case T1LH:
		return t1lh;
	case T2CL:
		clear_int(IF_T2);
		return t2c & 0xff;
	case T2CH:
		return t2c >> 8;
	case SR:
		val = sr;
		sr_start();
		return val;
	case ACR:
		return acr;
	case PCR:
		return pcr;
	case IFR:
		return ifr;
	case IER:
		return ier | 0x80;
	case ORA_NH:
	default:
		return read_ira();
	}
}

// What read() returns, leaving the chip alone: no CA/CB or timer flag
// cleared, no CA2 handshake, no shift started.
Byte mos6522::peek(Word offset)
{
	switch (offset & 0x0f) {
	case ORB:
		return read_irb();
	case ORA:
		return read_ira();
	case T1CL:
		return t1c & 0xff;
	case T2CL:
		return t2c & 0xff;
	case SR:
		return sr;
	default:
		return read(offset);
	}
}

void mos6522::write(Word offset, Byte val)
{
	switch (offset & 0x0f) {
	case ORB:
		orb = val;
		clear_int(cb_flags(pcr));
		cb2_handshake();
		break;
	case ORA:
		ora = val;
		clear_int(ca_flags(pcr));
		ca2_handshake();
		break;
	case DDRB:
		ddrb = val;
		break;
	case DDRA:
		ddra = val;
		break;
	case T1CL:
	case T1LL:
		t1ll = val;
		break;
	case T1CH:
		// load the counter and start it: PB7 goes low for the count
		t1lh = val;
		t1c = t1ll | (t1lh << 8);
		t1_reload = false;
		t1_armed = true;
		pb7 = false;
		clear_int(IF_T1);
		break;
	case T1LH:
		t1lh = val;
		clear_int(IF_T1);
		break;
	case T2CL:
		t2ll = val;
		break;
	case T2CH:
		t2c = t2ll | (val << 8);
		t2_armed = true;
		clear_int(IF_T2);
		break;
	case SR:
		sr = val;
		sr_start();
		break;
	case ACR:
		acr = val;
		if (!sr_internal_clock())
			out_cb1 = true;
		if (sr_mode() == 0)
			sr_running = false;
		if (sr_wait == 0)
			sr_wait = 1;
		break;
	case PCR:
		pcr = val;
		if ((pcr & 0x0c) == 0x0c)
			out_ca2 = (pcr & 0x02) != 0;
		if ((pcr & 0xc0) == 0xc0 && sr_mode() < 4)
			out_cb2 = (pcr & 0x20) != 0;
		break;
	case IFR:
		clear_int(val & 0x7f);
		break;
	case IER:
		if (val & 0x80)
			ier |= val & 0x7f;
		else
			ier &= ~val;
		update_irq();
		break;
	case ORA_NH:
		ora = val;
		break;
	}
}
