//
//	test_mos6522.cpp
//	Unit tests for mos6522, the VIA at $FF20-$FF2F, driven register
//	by register with the cycle count advanced by hand, so every timer
//	edge lands on a known cycle.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>

#include "mos6522.h"

static int failures = 0;

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
				__FILE__, __LINE__, #cond);		\
			++failures;					\
		}							\
	} while (0)

#define CHECK_EQ(a, b)							\
	do {								\
		const long _a = (long)(a), _b = (long)(b);		\
		if (_a != _b) {						\
			fprintf(stderr, "FAIL %s:%d: %s == %s "		\
				"($%02lX != $%02lX)\n",			\
				__FILE__, __LINE__, #a, #b, _a, _b);	\
			++failures;					\
		}							\
	} while (0)

using via = mos6522;

// the ~IRQ pin reads false while the VIA is interrupting
static bool irq(const mos6522& v)
{
	return !(bool)v.IRQ;
}

static void test_ports()
{
	mos6522 v;

	// nothing driving the inputs: the pull-ups read high
	CHECK_EQ(v.read(via::ORA), 0xff);
	CHECK_EQ(v.read(via::ORB), 0xff);

	// outputs read back from the output register, inputs from the pins
	v.write(via::DDRA, 0x0f);
	v.write(via::ORA, 0x5a);
	v.set_pa(0x30);
	CHECK_EQ(v.read(via::ORA), 0x3a);
	CHECK_EQ(v.pa(), 0x3a);
	CHECK_EQ(v.pa_driven(), 0x0f);

	v.write(via::DDRB, 0xf0);
	v.write(via::ORB, 0xa5);
	v.set_pb(0x0c);
	CHECK_EQ(v.read(via::ORB), 0xac);
	CHECK_EQ(v.read(via::DDRB), 0xf0);
	CHECK_EQ(v.read(via::DDRA), 0x0f);

	// reset clears the registers but leaves the far side alone
	v.reset();
	CHECK_EQ(v.read(via::DDRA), 0x00);
	CHECK_EQ(v.read(via::ORA), 0x30);
}

static void test_interrupt_enable()
{
	mos6522 v;

	v.set_ca1(false);	// PCR0 = 0: the falling edge is active
	CHECK_EQ(v.read(via::IFR), via::IF_CA1);
	CHECK(!irq(v));		// flagged but not enabled

	v.write(via::IER, 0x80 | via::IF_CA1);
	CHECK_EQ(v.read(via::IER), 0x80 | via::IF_CA1);
	CHECK_EQ(v.read(via::IFR), 0x80 | via::IF_CA1);
	CHECK(irq(v));

	// writing a one to an IFR bit clears it
	v.write(via::IFR, via::IF_CA1);
	CHECK_EQ(v.read(via::IFR), 0x00);
	CHECK(!irq(v));

	// IER bit 7 clear disables the bits given
	v.write(via::IER, via::IF_CA1);
	CHECK_EQ(v.read(via::IER), 0x80);
}

static void test_ca1_latch_and_flags()
{
	mos6522 v;

	v.write(via::PCR, 0x01);	// CA1 rising edge
	v.write(via::ACR, 0x01);	// latch PA on it

	v.set_ca1(false);		// the wrong edge
	CHECK_EQ(v.read(via::IFR), 0x00);

	v.set_pa(0x42);
	v.set_ca1(true);
	CHECK_EQ(v.read(via::IFR), via::IF_CA1);

	// the latch holds the value at the edge
	v.set_pa(0x99);
	CHECK_EQ(v.read(via::ORA_NH), 0x42);
	CHECK_EQ(v.read(via::IFR), via::IF_CA1);	// no-handshake read keeps the flag
	CHECK_EQ(v.read(via::ORA), 0x42);
	CHECK_EQ(v.read(via::IFR), 0x00);		// ORA read clears it
}

static void test_ca2_modes()
{
	mos6522 v;

	// independent interrupt input, falling edge: a port read leaves it
	v.write(via::PCR, 0x02);
	v.set_ca2(false);
	CHECK_EQ(v.read(via::IFR), via::IF_CA2);
	v.read(via::ORA);
	CHECK_EQ(v.read(via::IFR), via::IF_CA2);
	v.write(via::IFR, via::IF_CA2);

	// plain input: a port read clears it
	v.set_ca2(true);
	v.write(via::PCR, 0x00);
	v.set_ca2(false);
	CHECK_EQ(v.read(via::IFR), via::IF_CA2);
	v.read(via::ORA);
	CHECK_EQ(v.read(via::IFR), 0x00);

	// manual output
	v.write(via::PCR, 0x0c);
	CHECK(v.ca2_driven());
	CHECK(!v.ca2());
	v.write(via::PCR, 0x0e);
	CHECK(v.ca2());

	// handshake: low from the ORA access to the CA1 active edge
	v.write(via::PCR, 0x08);
	v.write(via::ORA, 0x00);
	CHECK(!v.ca2());
	v.tick(10);
	CHECK(!v.ca2());
	v.set_ca1(false);
	CHECK(v.ca2());

	// pulse: low for one cycle
	v.write(via::PCR, 0x0a);
	v.read(via::ORA);
	CHECK(!v.ca2());
	v.tick(1);
	CHECK(v.ca2());
}

static void test_cb2_handshake()
{
	mos6522 v;

	// CB2 handshakes on a write of ORB only
	v.write(via::PCR, 0x80);
	v.read(via::ORB);
	CHECK(v.cb2());
	v.write(via::ORB, 0x00);
	CHECK(!v.cb2());
	v.set_cb1(false);
	CHECK(v.cb2());
	CHECK_EQ(v.read(via::IFR), via::IF_CB1);
}

static void test_t1_one_shot()
{
	mos6522 v;

	v.write(via::IER, 0x80 | via::IF_T1);
	v.write(via::ACR, 0x80);	// PB7 output, one-shot
	v.write(via::T1CL, 0x10);
	v.write(via::T1CH, 0x00);	// N = 16
	CHECK(!(v.pb() & 0x80));	// PB7 low for the count
	CHECK(v.pb_driven() & 0x80);

	v.tick(16);
	CHECK_EQ(v.read(via::T1CH), 0x00);
	CHECK_EQ(v.read(via::T1CL), 0x00);
	CHECK(!irq(v));

	v.tick(1);			// N+1: through zero
	CHECK(irq(v));
	CHECK(v.pb() & 0x80);

	// reading T1C-L clears the flag; one-shot stays quiet after
	CHECK_EQ(v.read(via::T1CL), 0xff);
	CHECK(!irq(v));
	v.tick(0x20000);
	CHECK(!irq(v));
}

static void test_t1_free_run()
{
	mos6522 v;

	v.write(via::ACR, 0xc0);	// free-run, PB7 output
	v.write(via::T1LL, 0x08);
	v.write(via::T1CH, 0x00);	// N = 8, period N+2

	v.tick(9);
	CHECK_EQ(v.read(via::IFR), via::IF_T1);
	CHECK(v.pb() & 0x80);
	v.write(via::IFR, via::IF_T1);

	for (int i = 0; i < 3; i++) {
		v.tick(9);
		CHECK_EQ(v.read(via::IFR), 0x00);
		v.tick(1);
		CHECK_EQ(v.read(via::IFR), via::IF_T1);
		CHECK_EQ((v.pb() >> 7) & 1, i & 1);	// toggles each period
		v.write(via::IFR, via::IF_T1);
	}

	// T1L-H written alone changes the next period, not this one
	v.tick(1);			// the reload
	v.write(via::T1LH, 0x01);
	v.tick(9);
	CHECK_EQ(v.read(via::IFR), via::IF_T1);
	v.write(via::IFR, via::IF_T1);
	v.tick(0x108 + 1);
	CHECK_EQ(v.read(via::IFR), 0x00);
	v.tick(1);
	CHECK_EQ(v.read(via::IFR), via::IF_T1);
}

static void test_t2()
{
	mos6522 v;

	v.write(via::T2CL, 0x05);
	v.write(via::T2CH, 0x00);	// N = 5
	v.tick(5);
	CHECK_EQ(v.read(via::IFR), 0x00);
	v.tick(1);
	CHECK_EQ(v.read(via::IFR), via::IF_T2);
	CHECK_EQ(v.read(via::T2CL), 0xff);	// clears the flag
	CHECK_EQ(v.read(via::IFR), 0x00);
	v.tick(0x10000);
	CHECK_EQ(v.read(via::IFR), 0x00);	// once per T2C-H write

	// pulse counting: PB6 falling edges, the cycle clock ignored
	v.write(via::ACR, 0x20);
	v.write(via::T2CL, 0x02);
	v.write(via::T2CH, 0x00);
	v.tick(100);
	CHECK_EQ(v.read(via::T2CL), 0x02);
	for (int i = 0; i < 3; i++) {
		CHECK_EQ(v.read(via::IFR), 0x00);
		v.set_pb(0xbf);
		v.set_pb(0xff);
	}
	CHECK_EQ(v.read(via::IFR), via::IF_T2);
}

static void test_shift_out_phi2()
{
	mos6522 v;

	v.write(via::ACR, 0x18);	// mode 6: out under phi2
	CHECK(v.cb2_driven());
	CHECK(v.cb1_driven());
	v.write(via::SR, 0x81);

	v.tick(1);
	CHECK(v.cb2());			// bit 7 first
	v.tick(6);
	CHECK_EQ(v.read(via::IFR), 0x00);
	v.tick(1);
	CHECK(v.cb2());			// bit 0 last
	CHECK_EQ(v.read(via::IFR), via::IF_SR);
	CHECK(v.cb1());			// CB1 idles high
}

static void test_shift_in_external()
{
	mos6522 v;

	v.write(via::ACR, 0x0c);	// mode 3: in under CB1
	CHECK(!v.cb1_driven());
	v.read(via::SR);		// start

	const Byte data = 0xa6;
	for (int bit = 7; bit >= 0; bit--) {
		v.set_cb2((data >> bit) & 1);
		v.set_cb1(false);
		v.set_cb1(true);	// CB2 taken on the rising edge
	}
	// CB1 and CB2 were active-edge inputs all along
	CHECK_EQ(v.read(via::IFR), via::IF_SR | via::IF_CB1 | via::IF_CB2);
	CHECK_EQ(v.read(via::SR), data);
}

static void test_shift_out_t2()
{
	mos6522 v;

	v.write(via::T2CL, 0x02);	// an edge every 4 cycles
	v.write(via::ACR, 0x14);	// mode 5: out under T2
	v.write(via::SR, 0x00);
	v.tick(4 * 15);
	CHECK_EQ(v.read(via::IFR), 0x00);
	v.tick(4);
	CHECK_EQ(v.read(via::IFR), via::IF_SR);
}

// A debugger's peek sees what a read would, and leaves the chip as it was
static void test_peek()
{
	mos6522 v;

	v.write(via::PCR, 0x09);	// CA1 rising; CA2 handshake output
	v.write(via::ACR, 0x01);	// latch PA on CA1
	v.set_ca1(false);
	v.set_pa(0x42);
	v.set_ca1(true);
	CHECK_EQ(v.peek(via::ORA), 0x42);
	CHECK_EQ(v.read(via::IFR), via::IF_CA1);	// flag kept
	CHECK(v.ca2());					// no handshake
	CHECK_EQ(v.read(via::ORA), 0x42);
	CHECK_EQ(v.read(via::IFR), 0x00);
	CHECK(!v.ca2());				// the read does both

	mos6522 cb;
	cb.write(via::PCR, 0x10);	// CB1 rising
	cb.set_cb1(false);
	cb.set_cb1(true);
	cb.peek(via::ORB);
	CHECK_EQ(cb.read(via::IFR), via::IF_CB1);
	cb.read(via::ORB);
	CHECK_EQ(cb.read(via::IFR), 0x00);

	mos6522 t;
	t.write(via::T1CL, 0x03);
	t.write(via::T1CH, 0x00);
	t.write(via::T2CL, 0x03);
	t.write(via::T2CH, 0x00);
	t.tick(5);
	CHECK_EQ(t.read(via::IFR), via::IF_T1 | via::IF_T2);
	const Byte t1 = t.peek(via::T1CL), t2 = t.peek(via::T2CL);
	CHECK_EQ(t.read(via::IFR), via::IF_T1 | via::IF_T2);	// both kept
	CHECK_EQ(t.read(via::T1CL), t1);
	CHECK_EQ(t.read(via::T2CL), t2);
	CHECK_EQ(t.read(via::IFR), 0x00);

	mos6522 s;
	s.write(via::ACR, 0x0c);	// in under CB1: a read starts it
	s.peek(via::SR);
	CHECK(!s.cb1_driven());
	for (int i = 0; i < 8; i++) {
		s.set_cb1(false);
		s.set_cb1(true);
	}
	CHECK_EQ(s.read(via::IFR) & via::IF_SR, 0x00);	// nothing started
}

int main()
{
	test_ports();
	test_interrupt_enable();
	test_ca1_latch_and_flags();
	test_ca2_modes();
	test_cb2_handshake();
	test_t1_one_shot();
	test_t1_free_run();
	test_t2();
	test_shift_out_phi2();
	test_shift_in_external();
	test_shift_out_t2();
	test_peek();

	if (failures) {
		fprintf(stderr, "test_mos6522: %d FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_mos6522: PASS\n");
	return EXIT_SUCCESS;
}
