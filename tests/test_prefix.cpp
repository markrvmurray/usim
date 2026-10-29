//
//	test_prefix.cpp
//	$10/$11 prefixes ahead of a byte their page does not define. Per
//	Atkinson's real-silicon measurements the 6809 ignores the prefix,
//	for one extra cycle, and runs the byte as a page-1 opcode; a byte
//	that is itself a prefix starts a new one. MAME's m6809 core instead
//	drops the byte and runs the next one. No MC6809 has been run to
//	settle it, so these expectations are provisional. The HD6309 traps
//	all of these instead.
//
//	NitrOS-9's cputype relies on this to tell the two apart: it runs
//	`fdb $104F` (CLRD on a 6309) with D=$FFFF and then tests B.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "mc6809.h"
#include "hd6309.h"
#include "memory.h"

static int failures = 0;

struct Result {
	Byte		a, b;
	Word		x, y, pc;
	uint64_t	cycles;
};

// runs one instruction from $0400 with A=B=$FF and X=Y=0
template <class CPU>
class Probe : public CPU {
public:
	Result exec(std::initializer_list<Byte> code)
	{
		auto ram = std::make_shared<RAM>(0x10000);
		this->attach(ram, 0x0000, 0x0000);
		Word at = 0x0400;
		for (Byte c : code)
			this->write(at++, c);
		this->write_word(0xFFF0, 0x2000);	// HD6309 illegal-instruction trap
		this->reset();
		this->pc = 0x0400;
		this->s = 0x8000;
		this->a = this->b = 0xFF;
		this->x = this->y = 0;
		const uint64_t before = this->total_cycles + this->cycles;
		this->run();
		return { this->a, this->b, this->x, this->y, this->pc,
			 this->total_cycles + this->cycles - before };
	}

protected:
	void post_exec() override { this->halt(); }
};

template <class CPU>
static Result run(std::initializer_list<Byte> code)
{
	Probe<CPU> cpu;
	return cpu.exec(code);
}

static void check(const char* name, const Result& got, Byte a, Byte b, Word x, Word y, Word pc)
{
	if (got.a != a || got.b != b || got.x != x || got.y != y || got.pc != pc) {
		fprintf(stderr, "FAIL %-22s got  A=%02X B=%02X X=%04X Y=%04X PC=%04X\n"
				"%-27s want A=%02X B=%02X X=%04X Y=%04X PC=%04X\n",
			name, got.a, got.b, got.x, got.y, got.pc, "", a, b, x, y, pc);
		++failures;
	}
}

static void check_cycles(const char* name, uint64_t got, uint64_t want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %-22s %llu cycles, want %llu\n", name,
			(unsigned long long)got, (unsigned long long)want);
		++failures;
	}
}

int main()
{
	// cputype's probe: the prefix is ignored and $4F runs as CLRA,
	// leaving B for the TSTB that follows
	const Result clra = run<mc6809>({ 0x4F });
	const Result probe = run<mc6809>({ 0x10, 0x4F, 0x5D });
	check("$10 $4F", probe, 0x00, 0xFF, 0x0000, 0x0000, 0x0402);
	check_cycles("$10 $4F", probe.cycles, clra.cycles + 1);
	check("$11 $4F", run<mc6809>({ 0x11, 0x4F, 0x5C }), 0x00, 0xFF, 0x0000, 0x0000, 0x0402);

	// a prefix after a prefix starts a new one
	const Result ldy = run<mc6809>({ 0x10, 0x8E, 0x12, 0x34 });
	check("LDY #$1234", ldy, 0xFF, 0xFF, 0x0000, 0x1234, 0x0404);
	const Result ldy2 = run<mc6809>({ 0x11, 0x10, 0x8E, 0x12, 0x34 });
	check("$11 LDY", ldy2, 0xFF, 0xFF, 0x0000, 0x1234, 0x0405);
	check_cycles("$11 LDY", ldy2.cycles, ldy.cycles + 1);
	check("$10 $10 $10 LDY", run<mc6809>({ 0x10, 0x10, 0x10, 0x8E, 0x12, 0x34 }),
		0xFF, 0xFF, 0x0000, 0x1234, 0x0406);
	// page 3 has no $8E, so both prefixes go and it runs as LDX
	check("$10 $11 $8E LDX", run<mc6809>({ 0x10, 0x11, 0x8E, 0x12, 0x34 }),
		0xFF, 0xFF, 0x1234, 0x0000, 0x0405);

	// $10 $20 is LBRA on a 6809, costing a page-2 branch's 6 cycles
	const Result lbra = run<mc6809>({ 0x10, 0x20, 0x01, 0x00 });
	check("$10 $20 LBRA", lbra, 0xFF, 0xFF, 0x0000, 0x0000, 0x0504);
	check_cycles("$10 $20 LBRA", lbra.cycles, 6);

	// the HD6309 traps them all to $2000 (via $FFF0); $104F is CLRD
	check("6309 CLRD", run<hd6309>({ 0x10, 0x4F, 0x5D }), 0x00, 0x00, 0x0000, 0x0000, 0x0402);
	for (auto code : { std::initializer_list<Byte>{ 0x10, 0x10, 0x8E, 0x12, 0x34 },
			   std::initializer_list<Byte>{ 0x11, 0x42, 0x5C },
			   std::initializer_list<Byte>{ 0x10, 0x20, 0x01, 0x00 },
			   std::initializer_list<Byte>{ 0x10, 0x42 } }) {
		const Result r = run<hd6309>(code);
		if (r.pc != 0x2000) {
			fprintf(stderr, "FAIL 6309 $%02X $%02X did not trap (PC=%04X)\n",
				code.begin()[0], code.begin()[1], r.pc);
			++failures;
		}
	}

	if (failures) {
		printf("test_prefix: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_prefix: PASS\n");
	return EXIT_SUCCESS;
}
