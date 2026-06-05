//
//	main_picothing.cpp
//	Pico-Thing MC6809 System Emulator
//
//	Logical memory map (as the CPU sees it):
//	  $0000-$DFFF   DAT-remappable RAM. 7 × 8KB pages translated
//	                through the active task's DAT entries. A DAT
//	                entry of $FF marks the page unavailable: any
//	                access traps NMI (reads return $FF, writes are
//	                dropped) — matches the pT board's bad-page trap.
//	  $E000-$FDFF   Never-remapped RAM, hardwired to physical
//	                $1E000-$1FDFF. Independent of DAT settings; the
//	                board logic forces this mapping.
//	  $FE00-$FEFF   DAT RAM (256 bytes — 32 tasks × 8 pages). Slots
//	                where N % 8 == 7 are unused because guest page 7
//	                is never-remapped; identity init leaves $FF in
//	                slot $FF (= $FEFF) but the board's fixed mapping
//	                already covers what that slot would have meant.
//	  $FF00-$FFBF   I/O space (currently only IDE at $FF00-$FF09;
//	                $FF0A-$FFBF unmapped, reads return $FF).
//	  $FFC0-$FFFF   Pi Pico supervisory region — everything from the
//	                DAT task register through ACIAs, tick timer,
//	                SystemWatchpoint, TraceCtl, FRAM (SHARED) and the
//	                6809 vector table.
//	      $FFC0    DAT task register
//	      $FFC4-5  Console ACIA (MC6850)
//	      $FFC6-7  Auxiliary ACIA (MC6850)
//	      $FFC8-9  50Hz tick timer
//	      $FFCA    SystemWatchpoint control (hardware-faithful)
//	      $FFCB    TraceCtl (emulator-only; pT hardware has nothing
//	               here. Debug-build firmware may poke it to gate
//	               --brk / --trace — see tracectl.h.)
//	      $FFD0-DF FRAM (file-backed; SHARED area)
//	      $FFE0-FF SystemWatchpoint vector + snippet shadow (holds
//	               the 6809 vectors at $FFF0-$FFFF; arming via $FFCA
//	               copies the BREAK snippet to $FFE0-$FFEF and swaps
//	               the NMI vector — see system_watchpoint.h)
//
//	CLI mirrors usim09batch where it makes sense:
//	  --timeout=N        cap at N instructions (rc 124 on hit, GNU timeout(1))
//	  --cycles           print total simulated cycles to stderr on exit
//	  --trace            per-instruction CPU trace
//	  --watch=ADDR       byte-change watchpoint with register dump
//	  --brk=ADDR[,ADDR,...]  register dump before any listed PC
//	  --brk-gated        --brk output starts disabled; guest pokes $FFCB
//	                     to toggle (0x00 off, 0x01 brk-on, 0x02 brk+trace)
//	  -d <disk.img>      Master disk image file for IDE
//	  -D <disk.img>      Slave disk image file for IDE (must exist; not
//	                     auto-created — a stray flag mustn't spawn a 0-
//	                     byte image). If omitted, the slave slot reads
//	                     status $00 ("no device") as on a one-drive bus.
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

//
// FRAMBacking — adapts PicoFRAM as a persistent backing for the
// SystemWatchpoint shadow. The watchpoint window is $FFE0-$FFFF
// (32 bytes); PicoFRAM holds 48 bytes spanning $FFD0-$FFFF, so window
// offset N maps to FRAM offset N + 16. The SHARED area at $FFD0-$FFDF
// is read/written through the FRAM device directly and isn't touched
// here.
//
class FRAMBacking : public WatchpointBacking {
	PicoFRAM&	fram;
public:
			FRAMBacking(PicoFRAM& f) : fram(f) {}
	void		store(Word off, Byte val) override   { fram.write(off + 16, val); }
	Byte		load_byte(Word off) override         { return fram.read(off + 16); }
};

//
// Watch / dump address spec.
//
//   phys=false : a 16-bit CPU (logical) address. Read via cpu.read(),
//                so it goes through the active task's DAT exactly as the
//                guest sees it — valid for the whole $0000-$FFFF space
//                INCLUDING the DAT page table at $FE00-$FEFF, the fixed
//                window $E000-$FDFF, and I/O. The catch: a translated
//                address ($0000-$DFFF) follows whichever task is current,
//                so it can't pin one physical page across a task switch.
//   phys=true  : a physical backing-store address (0 .. 2MB-1), read via
//                DATRAM::read_physical(), bypassing the DAT entirely. Use
//                this to watch/dump a specific physical page regardless of
//                which task is mapped — e.g. one task's user block.
//
struct WatchSpec {
	bool		phys;
	unsigned long	addr;
	Byte		prev;
};

struct DumpSpec {
	bool		phys;
	unsigned long	addr;
	unsigned long	len;
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
		"  --trace-from=N      enable trace at instruction N (windowed)\n"
		"  --trace-to=N        disable trace at instruction N\n"
		"  --watch=[p:]ADDR    watch a byte for changes (repeatable).\n"
		"                      ADDR is a 16-bit CPU address (DAT-translated\n"
		"                      as the guest sees it, incl. DAT RAM $FE00-\n"
		"                      $FEFF). Prefix p: for a physical backing-\n"
		"                      store address (bypasses the DAT), e.g.\n"
		"                      --watch=p:0x15F55 to watch one task's page.\n"
		"  --dump=[p:]ADDR[,LEN]  hex+ASCII dump (repeatable). Same address\n"
		"                      forms as --watch (LEN default 256). Dumped at\n"
		"                      every --brk hit and once at exit/timeout.\n"
		"  --brk=ADDR[,...]    dump registers before listed PCs\n"
		"  --brk-gated         start with --brk output disabled; guest\n"
		"                      pokes $FFCB to toggle (0x00 off, 0x01 brk,\n"
		"                      0x02 brk+trace)\n"
		"  -d <disk.img>       master disk image for IDE\n"
		"  -D <disk.img>       slave  disk image for IDE (must exist;\n"
		"                      not auto-created. Omit for a one-drive bus.)\n"
		"  -f <fram.dat>       FRAM persistence file (default: picothing.fram)\n"
		"\n"
		"Firmware may be Intel HEX (.hex) or Motorola S-record (.s19/.srec).\n",
		prog);
}

int main(int argc, char* argv[])
{
	const char*		firmware_path = nullptr;
	const char*		disk_path = nullptr;
	const char*		slave_path = nullptr;
	const char*		fram_path = "picothing.fram";

	unsigned long		timeout = 0;
	bool			report_cycles = false;
	bool			trace = false;
	unsigned long		trace_from = 0;		// 0 = unset
	unsigned long		trace_to = 0;		// 0 = unset
	std::vector<WatchSpec>	watches;
	std::vector<DumpSpec>	dumps;
	std::vector<Word>	brk_addrs;
	bool			brk_gated = false;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--timeout=", 10) == 0) {
			timeout = strtoul(argv[i] + 10, nullptr, 10);
		} else if (strcmp(argv[i], "--cycles") == 0) {
			report_cycles = true;
		} else if (strcmp(argv[i], "--trace") == 0) {
			trace = true;
		} else if (strncmp(argv[i], "--trace-from=", 13) == 0) {
			trace_from = strtoul(argv[i] + 13, nullptr, 0);
		} else if (strncmp(argv[i], "--trace-to=", 11) == 0) {
			trace_to = strtoul(argv[i] + 11, nullptr, 0);
		} else if (strncmp(argv[i], "--watch=", 8) == 0) {
			const char* v = argv[i] + 8;
			bool phys = false;
			if (strncmp(v, "p:", 2) == 0) { phys = true; v += 2; }
			watches.push_back({ phys, strtoul(v, nullptr, 0), 0 });
		} else if (strncmp(argv[i], "--dump=", 7) == 0) {
			const char* v = argv[i] + 7;
			bool phys = false;
			if (strncmp(v, "p:", 2) == 0) { phys = true; v += 2; }
			char* end;
			unsigned long a = strtoul(v, &end, 0);
			unsigned long len = (*end == ',') ? strtoul(end + 1, nullptr, 0) : 256;
			dumps.push_back({ phys, a, len });
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
		} else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
			slave_path = argv[++i];
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

	// Pico-Thing constants. Logical map:
	//   $0000-$DFFF  DAT-translated (7 pages × 8KB)
	//   $E000-$FDFF  Never-remapped, fixed at physical $1E000-$1FDFF
	//   $FE00-$FEFF  DAT page table
	const Word	dat_addr_space_end  = 0xFE00;		// where DAT table starts
	const Word	fixed_window_start  = 0xE000;		// $E000-$FDFF
	const Word	fixed_window_size   = 0xFE00 - 0xE000;	// $1E00 bytes
	const size_t	fixed_window_phys   = 0x1E000;		// physical $1E000-$1FDFF
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

	auto datram     = std::make_shared<DATRAM>(dat_addr_space_end, physical_ram_size, dat_ram_size);
	datram->set_fixed_window(fixed_window_start, fixed_window_size, fixed_window_phys);
	auto datram_tk  = std::make_shared<DATRAMTicker>(*datram);
	auto taskreg    = std::make_shared<PicoTask>(*datram);
	auto console    = std::make_shared<mc6850>(console_term);
	auto aux_acia   = std::make_shared<mc6850>(aux_term);
	auto tick       = std::make_shared<PicoTick>(20000);	// 50Hz at ~1MHz
	auto ide        = std::make_shared<PicoIDE>(disk_path, slave_path);
	auto fram       = std::make_shared<PicoFRAM>(fram_path);
	auto tracectl   = std::make_shared<TraceCtl>(cpu, &brk_enabled);
	auto watch      = std::make_shared<SystemWatchpoint>(cpu);
	auto watch_ctrl = std::make_shared<SystemWatchpointCtrl>(*watch);
	auto watch_vec  = std::make_shared<SystemWatchpointVec>(*watch);
	auto fram_back  = std::make_shared<FRAMBacking>(*fram);

	// Wire FRAM as the watchpoint's persistent backing. set_backing
	// seeds the shadow from FRAM (sensible default if firmware load
	// is empty or skipped), and from this point every shadow mutation
	// — load(), disarmed vec_write(), arm()/disarm()/reset() vector
	// management — mirrors through to the .fram file. The firmware
	// load below then unconditionally writes $FFD0-$FFFF byte-for-
	// byte, matching the destructive DMA the Pico does on real boot.
	// Net effect: writes during the run are persisted; reload from
	// the same firmware wipes them, exactly like the hardware.
	watch->set_backing(fram_back.get());

	// IO devices first (scanned in order, first match wins).
	// Pico-thing peripherals don't sit on power-of-2 boundaries, so
	// they attach by (base, size) range rather than (base, mask).
	cpu.attach(watch);				// ActiveDevice: reset disarms
	cpu.attach(datram_tk);				// ActiveDevice: advances DATRAM NMI pulse
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
	// NMI is wired-OR (active-low) between the SystemWatchpoint and
	// DATRAM's "page-unavailable" trap. Each OutputPin reads false
	// when its source asserts; AND-ing the pins gives false → CPU
	// sees NMI asserted whenever either source trips.
	cpu.NMI.bind([&]() {
		return (bool)watch->NMI && (bool)datram->NMI;
	});

	// Load firmware via a temporary 64K loader so the same image can
	// populate physically separate devices: guest $0000-$FDFF into
	// DATRAM (translated zone identity-mapped to physical $0-$DFFF,
	// fixed zone $E000-$FDFF routed to physical $1E000-$1FDFF by
	// DATRAM's own write()), $FFD0-$FFDF into FRAM, and $FFE0-$FFFF
	// into the SystemWatchpoint shadow. Skipping zero bytes preserves
	// the zero-init of unwritten backing store and saves a load pass.
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

		// Route load through datram->write() so the fixed-window
		// translation ($E000-$FDFF → $1E000-$1FDFF) is honoured. DAT
		// is in identity-init state at load time, so the translated
		// zone resolves to identity physical addresses for task 0.
		// Range stops at $FE00 to avoid clobbering the DAT page
		// table; the SHARED + vector regions are loaded separately
		// into FRAM and the watchpoint shadow below.
		for (unsigned addr = 0; addr < dat_addr_space_end; addr++) {
			Byte b = loader->read(addr);
			if (b != 0x00)
				datram->write((Word)addr, b);
		}

		// SHARED area (16 bytes) and vector/snippet area (32 bytes)
		// are loaded destructively, byte for byte — same as the Pico
		// DMA does on real hardware. The previous run's persisted
		// state is overwritten unconditionally by the firmware image
		// (zero bytes for uncovered addresses included; we have no
		// way to distinguish "firmware wrote 0" from "firmware didn't
		// touch this byte" without per-byte coverage tracking in the
		// loaders). Intra-run persistence is what FRAMBacking buys:
		// any guest write to $FFD0-$FFFF during the run tees through
		// to .fram immediately, so the file reflects current state at
		// host shutdown.
		for (unsigned addr = 0xFFD0; addr <= 0xFFDF; addr++) {
			fram->write(addr - 0xFFD0, loader->read(addr));
		}
		for (unsigned addr = 0xFFE0; addr <= 0xFFFF; addr++) {
			watch->load((Word)addr, loader->read(addr));
		}
	}

	cpu.reset();
	if (trace) cpu.tron();

	// Read one byte either physically (bypassing the DAT) or as the
	// CPU sees it (DAT-translated). A physical read past the backing
	// store, or a CPU read, both fall back to $FF on miss.
	auto read_byte = [&](bool phys, unsigned long a) -> Byte {
		return phys ? datram->read_physical((size_t)a)
			    : cpu.read((Word)a);
	};

	// Hex + ASCII dump of every --dump spec. Called at each --brk hit
	// and once at exit, so a dump pinned to a physical page shows that
	// page's state at the moment of interest.
	auto do_dumps = [&]() {
		for (const auto& ds : dumps) {
			for (unsigned long off = 0; off < ds.len; off += 16) {
				fprintf(stderr, "%s $%06lX:",
					ds.phys ? "PHYS" : "MEM ", ds.addr + off);
				char ascii[17];
				int n = 0;
				for (int i = 0; i < 16 && off + i < ds.len; i++) {
					Byte b = read_byte(ds.phys, ds.addr + off + i);
					fprintf(stderr, " %02X", (unsigned)b);
					ascii[n++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
				}
				ascii[n] = '\0';
				fprintf(stderr, "  |%s|\n", ascii);
			}
		}
	};

	// Step loop is engaged when any of --timeout, --watch, --dump, --brk
	// are active. --brk-gated implies --brk semantics even without --brk
	// addresses, but on its own it's a no-op until the guest pokes
	// $FFCB and there are addresses to fire on.
	bool stepping = (timeout > 0) || !watches.empty()
		     || !dumps.empty() || !brk_addrs.empty()
		     || trace_from || trace_to;

	for (auto& w : watches) {
		w.prev = read_byte(w.phys, w.addr);
		fprintf(stderr, "WATCH: %s$%0*lX initial=%02X\n",
			w.phys ? "p:" : "", w.phys ? 6 : 4, w.addr, (unsigned)w.prev);
	}

	if (stepping) {
		unsigned long count = 0;
		while (timeout == 0 || count < timeout) {
			// Windowed trace: enable per-instruction trace for the
			// instruction-count window [trace_from, trace_to). Lets a
			// long multitasking run be traced in a tight window without
			// the whole-run volume (U-058 diagnosis).
			if (trace_from && count == trace_from) cpu.tron();
			if (trace_to && count == trace_to) cpu.troff();
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
						do_dumps();
						break;
					}
				}
			}
			cpu.tick();
			for (auto& w : watches) {
				Byte cur = read_byte(w.phys, w.addr);
				if (cur != w.prev) {
					Byte next = read_byte(w.phys, w.addr + 1);
					fprintf(stderr, "WATCH: %s$%0*lX %02X->%02X (word=%02X%02X) "
						"PC=$%04X A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X "
						"[insn#%lu]\n",
						w.phys ? "p:" : "", w.phys ? 6 : 4, w.addr,
						(unsigned)w.prev, (unsigned)cur, (unsigned)cur, (unsigned)next,
						(unsigned)cpu.get_insn_pc(),
						(unsigned)cpu.get_a(), (unsigned)cpu.get_b(),
						(unsigned)cpu.get_x(), (unsigned)cpu.get_y(),
						(unsigned)cpu.get_u(), (unsigned)cpu.get_s(),
						count);
					w.prev = cur;
				}
			}
			++count;
		}
		do_dumps();
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
