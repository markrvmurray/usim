//
//	test_exgtfr.cpp
//	MC6809 TFR and EXG against MAME's m6809 core: undefined register
//	selectors, mixed 8/16-bit transfers and the CC/DP byte duplication
//	that differs between TFR and the two orders of EXG.
//
//	The undefined-selector cases are the ones software relies on to
//	tell a 6809 from a 6309: `ldd #0 / tfr d,w / tfr w,d` leaves D at
//	$FFFF on a 6809, because W does not exist there.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>

#include "mc6809.h"
#include "memory.h"

static int failures = 0;

struct Regs {
	Byte	a, b, cc, dp;
	Word	x, y, u, s;
};

// runs one instruction from $0400 with the given registers
class Probe : public mc6809 {
public:
	Regs exec(Byte op, Byte post, const Regs& in)
	{
		auto ram = std::make_shared<RAM>(0x10000);
		attach(ram, 0x0000, 0x0000);
		write(0x0400, op);
		write(0x0401, post);
		reset();
		pc = 0x0400;
		a = in.a; b = in.b; cc.value = in.cc; dp = in.dp;
		x = in.x; y = in.y; u = in.u; s = in.s;
		run();
		return { a, b, (Byte)cc.value, dp, x, y, u, s };
	}

protected:
	void post_exec() override { halt(); }
};

static const Regs IN = { 0xA1, 0xB2, 0x45, 0x3C, 0x1234, 0x5678, 0x9ABC, 0xDEF0 };

static void check(const char* name, Byte op, Byte post, Regs want)
{
	Probe cpu;
	const Regs got = cpu.exec(op, post, IN);
	if (got.a != want.a || got.b != want.b || got.cc != want.cc ||
	    got.dp != want.dp || got.x != want.x || got.y != want.y ||
	    got.u != want.u || got.s != want.s) {
		fprintf(stderr, "FAIL %-12s got  A=%02X B=%02X CC=%02X DP=%02X X=%04X Y=%04X U=%04X S=%04X\n"
				"%-17s want A=%02X B=%02X CC=%02X DP=%02X X=%04X Y=%04X U=%04X S=%04X\n",
			name, got.a, got.b, got.cc, got.dp, got.x, got.y, got.u, got.s,
			"", want.a, want.b, want.cc, want.dp, want.x, want.y, want.u, want.s);
		++failures;
	}
}

// IN with some registers replaced
static Regs with(void (*edit)(Regs&))
{
	Regs r = IN;
	edit(r);
	return r;
}

static const Byte TFR = 0x1F, EXG = 0x1E;

int main()
{
	// defined registers still transfer as before
	check("TFR X,Y",   TFR, 0x12, with([](Regs& r) { r.y = 0x1234; }));
	check("TFR A,B",   TFR, 0x89, with([](Regs& r) { r.b = 0xA1; }));
	check("EXG X,U",   EXG, 0x13, with([](Regs& r) { r.x = 0x9ABC; r.u = 0x1234; }));
	check("EXG A,DP",  EXG, 0x8B, with([](Regs& r) { r.a = 0x3C; r.dp = 0xA1; }));

	// an undefined register reads $FFFF and ignores writes
	check("TFR D,W",   TFR, 0x06, IN);
	check("TFR W,D",   TFR, 0x60, with([](Regs& r) { r.a = 0xFF; r.b = 0xFF; }));
	check("TFR W,Y",   TFR, 0x62, with([](Regs& r) { r.y = 0xFFFF; }));
	check("TFR V,X",   TFR, 0x71, with([](Regs& r) { r.x = 0xFFFF; }));
	check("TFR $E,A",  TFR, 0xE8, with([](Regs& r) { r.a = 0xFF; }));
	check("TFR $F,CC", TFR, 0xFA, with([](Regs& r) { r.cc = 0xFF; }));
	check("TFR X,$C",  TFR, 0x1C, IN);
	check("EXG X,W",   EXG, 0x16, with([](Regs& r) { r.x = 0xFFFF; }));
	check("EXG W,B",   EXG, 0x69, with([](Regs& r) { r.b = 0xFF; }));

	// 8 to 16 bits: A and B gain $FF high; TFR duplicates CC and DP
	check("TFR A,X",   TFR, 0x81, with([](Regs& r) { r.x = 0xFFA1; }));
	check("TFR B,S",   TFR, 0x94, with([](Regs& r) { r.s = 0xFFB2; }));
	check("TFR CC,X",  TFR, 0xA1, with([](Regs& r) { r.x = 0x4545; }));
	check("TFR DP,D",  TFR, 0xB0, with([](Regs& r) { r.a = 0x3C; r.b = 0x3C; }));

	// 16 to 8 bits: the low byte
	check("TFR X,A",   TFR, 0x18, with([](Regs& r) { r.a = 0x34; }));
	check("TFR Y,DP",  TFR, 0x2B, with([](Regs& r) { r.dp = 0x78; }));

	// EXG writes the second register first, which decides an exchange
	// between D and one of its own halves
	check("EXG A,D",   EXG, 0x80, with([](Regs& r) { r.a = 0xB2; r.b = 0xA1; }));
	check("EXG D,A",   EXG, 0x08, with([](Regs& r) { r.a = 0xFF; r.b = 0xA1; }));

	// EXG with an 8-bit first register duplicates CC and DP; with a
	// 16-bit first register they read $FF high
	check("EXG CC,X",  EXG, 0xA1, with([](Regs& r) { r.x = 0x4545; r.cc = 0x34; }));
	check("EXG X,CC",  EXG, 0x1A, with([](Regs& r) { r.x = 0xFF45; r.cc = 0x34; }));
	check("EXG DP,Y",  EXG, 0xB2, with([](Regs& r) { r.y = 0x3C3C; r.dp = 0x78; }));
	check("EXG Y,DP",  EXG, 0x2B, with([](Regs& r) { r.y = 0xFF3C; r.dp = 0x78; }));

	if (failures) {
		printf("test_exgtfr: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_exgtfr: PASS\n");
	return EXIT_SUCCESS;
}
