//
//	test_neg.cpp
//	MC6809 NEG, every operand in every form: NEGA, NEGB, and NEG
//	direct, indexed and extended. The result is 0 - M; N and Z follow
//	it, V is set only for $80, and C is the borrow, so it is clear
//	only for 0 (MC6809 Programming Manual, NEG). H is undefined and
//	not checked; E, F and I must be left alone.
//
//	The carry matters beyond NEG itself: compilers negate D as
//	NEGA; NEGB; SBCA #0, which is right only if NEGB borrows for
//	every non-zero B. That sequence is checked for every value of D.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>

#include "mc6809.h"
#include "memory.h"

static int failures = 0;

// runs a few instructions from $0400
class Probe : public mc6809 {
public:
	using mc6809::a;
	using mc6809::b;
	using mc6809::x;
	using mc6809::dp;
	using mc6809::cc;

	std::shared_ptr<RAM>	ram = std::make_shared<RAM>(0x10000);

	Probe()
	{
		attach(ram, 0x0000, 0x0000);
		reset();
	}

	void load(std::initializer_list<Byte> code)
	{
		Word	at = 0x0400;
		for (Byte op : code)
			ram->write(at++, op);
	}

	void exec(unsigned n)
	{
		pc = 0x0400;
		left = n;
		run();
	}

protected:
	unsigned	left = 0;
	void post_exec() override { if (--left == 0) halt(); }
};

static const Byte E = 0x80, F = 0x40, H = 0x20, I = 0x10, N = 0x08, Z = 0x04, V = 0x02, C = 0x01;

// what NEG of m leaves, by the manual
static Byte want_result(Byte m) { return (Byte)(0 - m); }

static Byte want_flags(Byte m, Byte cc_in)
{
	const Byte r = want_result(m);
	return (cc_in & (E | F | I)) |
		((r & 0x80) ? N : 0) | (r == 0 ? Z : 0) |
		(m == 0x80 ? V : 0) | (m != 0 ? C : 0);
}

static void check(const char* form, Byte m, Byte cc_in, Byte got, Byte cc_out)
{
	const Byte r = want_result(m), f = want_flags(m, cc_in);
	if (got != r || (cc_out & ~H) != f) {
		if (failures < 20)
			fprintf(stderr, "FAIL %-8s M=%02X CC in=%02X: got %02X CC=%02X, want %02X CC=%02X (H ignored)\n",
				form, m, cc_in, got, cc_out, r, f);
		++failures;
	}
}

int main()
{
	Probe		cpu;
	static const Byte cc_ins[] = { 0x00, 0xFF, C, (Byte)~C };

	for (Byte cc_in : cc_ins) {
		for (unsigned v = 0; v < 256; ++v) {
			const Byte m = (Byte)v;

			cpu.load({ 0x40 });				// NEGA
			cpu.a = m; cpu.b = 0x5A; cpu.cc.value = cc_in;
			cpu.exec(1);
			check("NEGA", m, cc_in, cpu.a, cpu.cc.value);
			if (cpu.b != 0x5A) { fprintf(stderr, "FAIL NEGA changed B\n"); ++failures; }

			cpu.load({ 0x50 });				// NEGB
			cpu.b = m; cpu.a = 0xA5; cpu.cc.value = cc_in;
			cpu.exec(1);
			check("NEGB", m, cc_in, cpu.b, cpu.cc.value);
			if (cpu.a != 0xA5) { fprintf(stderr, "FAIL NEGB changed A\n"); ++failures; }

			cpu.load({ 0x00, 0x40 });			// NEG <$40, DP=$20
			cpu.dp = 0x20; cpu.ram->write(0x2040, m); cpu.cc.value = cc_in;
			cpu.exec(1);
			check("direct", m, cc_in, cpu.ram->read(0x2040), cpu.cc.value);

			cpu.load({ 0x60, 0x84 });			// NEG ,X
			cpu.x = 0x3000; cpu.ram->write(0x3000, m); cpu.cc.value = cc_in;
			cpu.exec(1);
			check("indexed", m, cc_in, cpu.ram->read(0x3000), cpu.cc.value);

			cpu.load({ 0x70, 0x10, 0x00 });			// NEG >$1000
			cpu.ram->write(0x1000, m); cpu.cc.value = cc_in;
			cpu.exec(1);
			check("extended", m, cc_in, cpu.ram->read(0x1000), cpu.cc.value);
		}
	}

	// NEGA; NEGB; SBCA #0 negates D, whatever C was before
	cpu.load({ 0x40, 0x50, 0x82, 0x00 });
	for (Byte cc_in : { (Byte)0x00, C }) {
		for (unsigned d = 0; d < 0x10000; ++d) {
			cpu.a = (Byte)(d >> 8); cpu.b = (Byte)d; cpu.cc.value = cc_in;
			cpu.exec(3);
			const Word got = (Word)(cpu.a << 8 | cpu.b), want = (Word)(0 - d);
			if (got != want) {
				if (failures < 20)
					fprintf(stderr, "FAIL NEGA;NEGB;SBCA #0 D=%04X C=%d: got %04X, want %04X\n",
						d, cc_in & C, got, want);
				++failures;
			}
		}
	}

	if (failures) {
		printf("test_neg: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_neg: PASS\n");
	return EXIT_SUCCESS;
}
