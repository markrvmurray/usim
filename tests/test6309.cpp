//
// test6309.cpp
// Functional test runner for the HD6309 emulator (hd6309 subclass).
//
// The test binary is loaded at $0400; the rest of RAM is zero.
// The program self-traps (branches to itself) when it finishes, which
// halts the CPU. It reports its result in the RESULT byte at $0010:
//   $00 = never ran, $FF = all tests passed, otherwise = failing test number.
//
// This same binary is meant to validate on MAME's llvm6309 driver too, so
// the pass/fail is carried in guest memory rather than a host-specific trap
// address.
//
// vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>

#include "hd6309.h"
#include "memory.h"

static constexpr Word RESULT_ADDR = 0x0010;
static constexpr Byte RESULT_PASS = 0xff;

static constexpr size_t HISTORY = 16;

class Test6309 : public hd6309 {
public:
	Word trap_pc = 0;
	Word history[HISTORY] = {};
	size_t hist_idx = 0;

protected:
	void post_exec() override {
		history[hist_idx++ % HISTORY] = insn_pc;
		if (pc == insn_pc) {
			trap_pc = insn_pc;
			halt();
		}
	}
};

int main(int argc, char *argv[])
{
	const char *binfile = (argc == 2) ? argv[1] : "tests/test6309.bin";

	Test6309 cpu;

	// flat 64KB RAM
	auto ram = std::make_shared<RAM>(0x10000);
	cpu.attach(ram, 0x0000, 0x0000);

	// load binary at $0400
	{
		FILE *f = fopen(binfile, "rb");
		if (!f) {
			fprintf(stderr, "error: cannot open %s\n", binfile);
			return EXIT_FAILURE;
		}
		Word addr = 0x0400;
		int ch;
		while ((ch = fgetc(f)) != EOF) {
			cpu.write(addr++, (Byte)ch);
		}
		fclose(f);
	}

	// lwasm's raw format does not pad out to the reset vector at $fffe, so
	// point it at the load address ($0400) for a deterministic start.
	cpu.write(0xfffe, 0x04);
	cpu.write(0xffff, 0x00);

	cpu.reset();
	cpu.run();

	Byte result = cpu.read(RESULT_ADDR);
	if (result == RESULT_PASS) {
		printf("PASS (PC=%04X)\n", cpu.trap_pc);
		return EXIT_SUCCESS;
	}

	printf("FAIL: test %u (trap PC=%04X)\n", (unsigned)result, cpu.trap_pc);
	cpu.print_regs();
	size_t n = std::min(cpu.hist_idx, HISTORY);
	size_t start = cpu.hist_idx - n;
	printf("Last %zu instructions:\n", n);
	for (size_t i = 0; i < n; ++i) {
		printf("  %04X\n", cpu.history[(start + i) % HISTORY]);
	}
	return EXIT_FAILURE;
}
