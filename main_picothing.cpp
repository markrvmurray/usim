//
//	main_picothing.cpp
//	Pico-Thing MC6809 System Emulator
//
//	Memory map:
//	  $0000-$FDFF   DAT-translated RAM (2MB, 8KB pages)
//	  $FE00-$FEFF   DAT RAM (256 bytes — 32 tasks * 8 pages)
//	  $FF00-$FF09   PATA IDE controller
//	  $FFC0         DAT task register
//	  $FFC4-$FFC5   Console ACIA (MC6850)
//	  $FFC6-$FFC7   Auxiliary ACIA (MC6850)
//	  $FFC8-$FFC9   50Hz tick timer
//	  $FFCA         SystemWatchpoint control register (hardware-faithful)
//	  $FFCB         TraceCtl (emulator-only; debug builds may poke
//	                this to gate --brk/--trace from the guest side —
//	                see tracectl.h. Real hardware has nothing here)
//	  $FFD0-$FFDF   Persistent RAM (FRAM, file-backed; SHARED area)
//	  $FFE0-$FFFF   SystemWatchpoint vector + snippet shadow (includes
//	                the 6809 vectors at $FFF0-$FFFF). Behaves as plain
//	                memory when disarmed; arming via $FFCA copies the
//	                BREAK snippet to $FFE0-$FFEF and swaps the NMI
//	                vector. See system_watchpoint.h.
//
//	CLI mirrors usim09batch where it makes sense:
//	  --timeout=N        cap at N instructions (rc 124 on hit, GNU timeout(1))
//	  --cycles           print total simulated cycles to stderr on exit
//	  --trace            per-instruction CPU trace
//	  --watch=ADDR       byte-change watchpoint with register dump
//	  --brk=ADDR[,ADDR,...]  register dump before any listed PC
//	  --brk-gated        --brk output starts disabled; guest pokes $FFCB
//	                     to toggle (0x00 off, 0x01 brk-on, 0x02 brk+trace)
//	  -d <disk.img>      Disk image file for IDE
//	  -f <fram.dat>      FRAM persistence file (default: picothing.fram)
//	  <firmware>         .hex / .s19 / .srec — autodetected by extension
//
//	No HaltDevice: pico-thing firmware has no host-exit mechanism;
//	the emulator runs until interrupted, until a watchpoint/breakpoint
//	is satisfied with --timeout, or until cpu.halt() is called.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <strings.h>
#include <vector>

#include "mc6809.h"
#include "mc6850.h"
#include "term.h"
#include "memory.h"
#include "picotask.h"
#include "picotick.h"
#include "picoide.h"
#include "picofram.h"
#include "tracectl.h"
#include "system_watchpoint.h"

//
// Null terminal for auxiliary ACIA — discards output, never has input
//
class NullTerminal : public mc6850_impl {
public:
	bool	poll_read() override { return false; }
	Byte	read() override { return 0xFF; }
	void	write(Byte) override { }
};

static void usage(const char* prog)
{
	fprintf(stderr,
		"usage: %s [options] <firmware>\n"
		"\n"
		"Options:\n"
		"  --timeout=N         cap at N instructions (rc 124 on hit)\n"
		"  --cycles            print total cycles to stderr on exit\n"
		"  --trace             per-instruction CPU trace\n"
		"  --watch=ADDR        watch byte at ADDR for changes\n"
		"  --brk=ADDR[,...]    dump registers before listed PCs\n"
		"  --brk-gated         start with --brk output disabled; guest\n"
		"                      pokes $FFCB to toggle (0x00 off, 0x01 brk,\n"
		"                      0x02 brk+trace)\n"
		"  -d <disk.img>       disk image for IDE\n"
		"  -f <fram.dat>       FRAM persistence file (default: picothing.fram)\n"
		"\n"
		"Firmware may be Intel HEX (.hex) or Motorola S-record (.s19/.srec).\n",
		prog);
}

int main(int argc, char* argv[])
{
	const char*		firmware_path = nullptr;
	const char*		disk_path = nullptr;
	const char*		fram_path = "picothing.fram";

	unsigned long		timeout = 0;
	bool			report_cycles = false;
	bool			trace = false;
	Word			watch_addr = 0;
	bool			watching = false;
	std::vector<Word>	brk_addrs;
	bool			brk_gated = false;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--timeout=", 10) == 0) {
			timeout = strtoul(argv[i] + 10, nullptr, 10);
		} else if (strcmp(argv[i], "--cycles") == 0) {
			report_cycles = true;
		} else if (strcmp(argv[i], "--trace") == 0) {
			trace = true;
		} else if (strncmp(argv[i], "--watch=", 8) == 0) {
			watch_addr = (Word)strtoul(argv[i] + 8, nullptr, 0);
			watching = true;
		} else if (strncmp(argv[i], "--brk=", 6) == 0) {
			char *p = argv[i] + 6;
			while (*p) {
				char *end;
				unsigned long a = strtoul(p, &end, 0);
				if (end == p) break;
				brk_addrs.push_back((Word)a);
				p = end;
				if (*p == ',') ++p;
			}
		} else if (strcmp(argv[i], "--brk-gated") == 0) {
			brk_gated = true;
		} else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			disk_path = argv[++i];
		} else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
			fram_path = argv[++i];
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
			return EXIT_FAILURE;
		} else {
			firmware_path = argv[i];
		}
	}

	if (!firmware_path) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	(void)signal(SIGINT, SIG_IGN);

	// Pico-Thing constants
	const Word	dat_translated_size = 0xFE00;		// $0000-$FDFF
	const size_t	physical_ram_size   = 2 * 1024 * 1024;	// 2MB
	const Word	dat_ram_size        = 0x0100;		// 256 bytes

	mc6809		cpu;
	Terminal	console_term(cpu);
	NullTerminal	aux_term;

	// --brk output is on by default; --brk-gated starts it disabled
	// and lets the guest toggle it via TraceCtl pokes at $FFCB. Real
	// pico-thing hardware has nothing at $FFCB, so this slot is safe
	// to repurpose for an emulator-only debugger backdoor.
	bool brk_enabled = !brk_gated;

	auto datram     = std::make_shared<DATRAM>(dat_translated_size, physical_ram_size, dat_ram_size);
	auto taskreg    = std::make_shared<PicoTask>(*datram);
	auto console    = std::make_shared<mc6850>(console_term);
	auto aux_acia   = std::make_shared<mc6850>(aux_term);
	auto tick       = std::make_shared<PicoTick>(20000);	// 50Hz at ~1MHz
	auto ide        = std::make_shared<PicoIDE>(disk_path);
	auto fram       = std::make_shared<PicoFRAM>(fram_path);
	auto tracectl   = std::make_shared<TraceCtl>(cpu, &brk_enabled);
	auto watch      = std::make_shared<SystemWatchpoint>(cpu);
	auto watch_ctrl = std::make_shared<SystemWatchpointCtrl>(*watch);
	auto watch_vec  = std::make_shared<SystemWatchpointVec>(*watch);

	// IO devices first (scanned in order, first match wins).
	// Pico-thing peripherals don't sit on power-of-2 boundaries, so
	// they attach by (base, size) range rather than (base, mask).
	cpu.attach(watch);				// ActiveDevice: reset disarms
	cpu.attach_range(ide,        0xFF00, 0x000A);	// $FF00-$FF09
	cpu.attach_range(taskreg,    0xFFC0, 0x0001);	// $FFC0
	cpu.attach_range(console,    0xFFC4, 0x0002);	// $FFC4-$FFC5
	cpu.attach_range(aux_acia,   0xFFC6, 0x0002);	// $FFC6-$FFC7
	cpu.attach_range(tick,       0xFFC8, 0x0002);	// $FFC8-$FFC9
	cpu.attach_range(watch_ctrl, 0xFFCA, 0x0001);	// $FFCA (hardware-faithful)
	cpu.attach_range(tracectl,   0xFFCB, 0x0001);	// $FFCB (emulator-only)
	cpu.attach_range(fram,       0xFFD0, 0x0010);	// $FFD0-$FFDF (SHARED area)
	cpu.attach_range(watch_vec,  0xFFE0, 0x0020);	// $FFE0-$FFFF (vectors + snippet)
	cpu.attach_range(datram,     0x0000, 0xFF00);	// $0000-$FEFF

	// Console ACIA + aux ACIA + tick timer all share the IRQ line
	// (matches real pico-thing hardware routing); SystemWatchpoint
	// drives NMI. FIRQ is unused.
	//
	// OutputPinReg uses invert=true for each ~IRQ pin, so the bool
	// reads false when the source has its IRQ status bit set. To
	// model the wired-OR (open-collector) ~IRQ line on the bus, AND
	// the pins: if any one asserts (false), the AND chain is false,
	// the CPU's c_irq is false, and !c_irq triggers do_irq.
	cpu.IRQ.bind([&]() {
		return (bool)console->IRQ && (bool)aux_acia->IRQ && (bool)tick->IRQ;
	});
	cpu.NMI.bind([&]() {
		return (bool)watch->NMI;
	});

	// Load firmware via a temporary 64K loader so the same image can
	// populate physically separate devices: identity-mapped $0000-$FEFF
	// into DATRAM's backing store, $FFD0-$FFDF into FRAM, and
	// $FFE0-$FFFF into the SystemWatchpoint shadow.
	//
	// Format is sniffed from the first byte rather than the extension:
	//   $7F  → ELF32 (llvm-mc6809 / lld output)
	//   'S'  → Motorola S-record
	//   ':'  → Intel HEX
	{
		auto loader = std::make_shared<RAM>(0x10000);
		FILE *probe = fopen(firmware_path, "rb");
		if (!probe) { perror(firmware_path); return EXIT_FAILURE; }
		unsigned char magic[4] = {0};
		(void)fread(magic, 1, 4, probe);
		fclose(probe);
		if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
			loader->load_elf(firmware_path, 0x0000);
		} else if (magic[0] == 'S') {
			loader->load_srec(firmware_path, 0x0000);
		} else {
			loader->load_intelhex(firmware_path, 0x0000);
		}

		for (unsigned addr = 0; addr < 0xFF00; addr++) {
			Byte b = loader->read(addr);
			if (b != 0x00)
				datram->write_physical(addr, b);
		}

		// SHARED area (16 bytes) into FRAM persistence.
		for (unsigned addr = 0xFFD0; addr <= 0xFFDF; addr++) {
			Byte b = loader->read(addr);
			fram->write(addr - 0xFFD0, b);
		}

		// Vector + snippet area (32 bytes) into the SystemWatchpoint
		// shadow. Note: this is not currently persisted to FRAM — a
		// run that updates a vector won't survive a host restart. The
		// hardware writes through to FRAM in place; emulating that
		// requires giving the watchpoint a reference to the FRAM
		// device (deferred — see CLAUDE.md).
		for (unsigned addr = 0xFFE0; addr <= 0xFFFF; addr++) {
			watch->load((Word)addr, loader->read(addr));
		}
	}

	cpu.reset();
	if (trace) cpu.tron();

	// Step loop is engaged when any of --timeout, --watch, --brk are
	// active. --brk-gated implies --brk semantics even without --brk
	// addresses, but on its own it's a no-op until the guest pokes
	// $FFCB and there are addresses to fire on.
	bool stepping = (timeout > 0) || watching || !brk_addrs.empty();

	Byte watch_prev = 0;
	if (watching) {
		watch_prev = cpu.read(watch_addr);
		fprintf(stderr, "WATCH: $%04X initial=%02X\n", watch_addr, watch_prev);
	}

	if (stepping) {
		unsigned long count = 0;
		while (timeout == 0 || count < timeout) {
			if (!brk_addrs.empty() && brk_enabled) {
				Word cur_pc = cpu.get_pc();
				for (Word a : brk_addrs) {
					if (cur_pc == a) {
						fprintf(stderr, "BRK: PC=$%04X "
							"A=%02X B=%02X X=%04X Y=%04X "
							"U=%04X S=%04X CC=%02X "
							"[insn#%lu]\n",
							(unsigned)cur_pc,
							(unsigned)cpu.get_a(), (unsigned)cpu.get_b(),
							(unsigned)cpu.get_x(), (unsigned)cpu.get_y(),
							(unsigned)cpu.get_u(), (unsigned)cpu.get_s(),
							(unsigned)cpu.get_cc(),
							count);
						break;
					}
				}
			}
			cpu.tick();
			if (watching) {
				Byte cur = cpu.read(watch_addr);
				if (cur != watch_prev) {
					Byte next = cpu.read(watch_addr + 1);
					fprintf(stderr, "WATCH: $%04X %02X->%02X (word=%02X%02X) "
						"PC=$%04X A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X "
						"[insn#%lu]\n",
						watch_addr, watch_prev, cur, cur, next,
						(unsigned)cpu.get_insn_pc(),
						(unsigned)cpu.get_a(), (unsigned)cpu.get_b(),
						(unsigned)cpu.get_x(), (unsigned)cpu.get_y(),
						(unsigned)cpu.get_u(), (unsigned)cpu.get_s(),
						count);
					watch_prev = cur;
				}
			}
			++count;
		}
		if (timeout > 0) {
			if (report_cycles) {
				fprintf(stderr, "cycles=%llu\n",
					(unsigned long long)cpu.get_total_cycles());
			}
			fprintf(stderr, "%s: timeout after %lu instructions\n",
				argv[0], timeout);
			return 124;
		}
	} else {
		cpu.run();
	}

	if (report_cycles) {
		fprintf(stderr, "cycles=%llu\n",
			(unsigned long long)cpu.get_total_cycles());
	}
	return EXIT_SUCCESS;
}
