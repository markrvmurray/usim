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

// --wlog spec: a physical (p:) or logical address range whose guest
// stores are logged non-perturbingly through DATRAM::write (U-067).
struct WlogSpec {
	bool		phys;
	unsigned long	addr;
	unsigned long	len;
};

// One recorded store: the canonical cycle, the PC of the storing
// instruction, the logical + physical target, and the old/new bytes.
struct WlogRec {
	uint64_t	cyc;
	Word		pc;
	Word		virt;
	size_t		phys;
	Byte		oldv;
	Byte		newv;
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
		"  --trace-task=ADDR   restrict trace+brk to instructions whose ACTIVE\n"
		"                      task TCB == ADDR (utask at $BF5B). Cuts through\n"
		"                      the relasmb/kernel PC overlap to trace one task.\n"
		"  --trace-procs       log kernel fork/exec/exit (TCB + exec'd fnumbr)\n"
		"                      so the program-at-TCB is knowable over time.\n"
		"  --tick=N            cycles per 50Hz timer tick (default 20000).\n"
		"                      Lower = more frequent IRQs: an amplifier for\n"
		"                      timer-IRQ-vs-kernel-window timing races.\n"
		"  --trap-sled         fire the crash post-mortem at the FIRST wild\n"
		"                      control transfer onto a $00 sled (catches the\n"
		"                      originating jump, not the final invalid op).\n"
		"  --shadow            track the last-writer (PC+cycle) of every\n"
		"                      physical byte; post-mortem reports who last\n"
		"                      wrote the corrupt resume-PC slot.\n"
		"  --watch=[p:]ADDR    watch a byte for changes (repeatable).\n"
		"                      ADDR is a 16-bit CPU address (DAT-translated\n"
		"                      as the guest sees it, incl. DAT RAM $FE00-\n"
		"                      $FEFF). Prefix p: for a physical backing-\n"
		"                      store address (bypasses the DAT), e.g.\n"
		"                      --watch=p:0x15F55 to watch one task's page.\n"
		"  --dump=[p:]ADDR[,LEN]  hex+ASCII dump (repeatable). Same address\n"
		"                      forms as --watch (LEN default 256). Dumped at\n"
		"                      every --brk hit and once at exit/timeout.\n"
		"  --wlog=[p:]ADDR[,LEN]  non-perturbing write-log (repeatable):\n"
		"                      report every guest store into the range with\n"
		"                      cycle+PC+old/new, adding ZERO guest cycles and\n"
		"                      no side-effects (preserves a timing-race crash).\n"
		"                      Same address forms as --watch (LEN default 1).\n"
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
	long			trace_task = -1;	// -1 = unset; else restrict trace+brk to instructions whose ACTIVE task TCB == this addr (U-080)
	bool			trace_procs = false;	// U-080: log fork/exec/exit so program-at-TCB is knowable over time
	unsigned long		tick_cycles = 20000;	// 50Hz at ~1MHz (default)
	unsigned long long	ctrace_from = 0, ctrace_to = 0;	// cycle-windowed trace
	bool			trap_sled = false;	// dump at first wild transfer onto a $00 sled
	bool			trap_spin = false;	// dump when trampoline entries go dense (spiral start)
	bool			detect_dbl = false;	// U-067 Detector A: page double-allocation
	bool			detect_stray = false;	// Detector B: stray write into another task / DAT
	bool			detect_free = false;	// getpag/givpag of a live task page
	bool			detect_iostk = false;	// U-067: I/O copy into the user-block/system-stack page
	bool			detect_srise = false;	// U-067: user S rising into the system-stack zone
	bool			detect_sphi = false;	// U-070: S corrupted into high memory ($FF00+)
	bool			track_shadow = false;	// full last-writer shadow (debug)
	std::vector<WatchSpec>	watches;
	std::vector<DumpSpec>	dumps;
	std::vector<WlogSpec>	wlogs;
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
		} else if (strncmp(argv[i], "--trace-task=", 13) == 0) {
			trace_task = (long)strtoul(argv[i] + 13, nullptr, 0);
		} else if (strcmp(argv[i], "--trace-procs") == 0) {
			trace_procs = true;
		} else if (strncmp(argv[i], "--tick=", 7) == 0) {
			tick_cycles = strtoul(argv[i] + 7, nullptr, 0);
			if (tick_cycles == 0) tick_cycles = 20000;
		} else if (strncmp(argv[i], "--ctrace-from=", 14) == 0) {
			ctrace_from = strtoull(argv[i] + 14, nullptr, 0);
		} else if (strncmp(argv[i], "--ctrace-to=", 12) == 0) {
			ctrace_to = strtoull(argv[i] + 12, nullptr, 0);
		} else if (strcmp(argv[i], "--trap-sled") == 0) {
			trap_sled = true;
		} else if (strcmp(argv[i], "--trap-spin") == 0) {
			trap_spin = true;
		} else if (strcmp(argv[i], "--detect-dbl") == 0) {
			detect_dbl = true;
		} else if (strcmp(argv[i], "--detect-stray") == 0) {
			detect_stray = true;
		} else if (strcmp(argv[i], "--detect-free") == 0) {
			detect_free = true;
		} else if (strcmp(argv[i], "--detect-iostk") == 0) {
			detect_iostk = true;
		} else if (strcmp(argv[i], "--detect-srise") == 0) {
			detect_srise = true;
		} else if (strcmp(argv[i], "--detect-sphi") == 0) {
			detect_sphi = true;
		} else if (strcmp(argv[i], "--shadow") == 0) {
			track_shadow = true;
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
		} else if (strncmp(argv[i], "--wlog=", 7) == 0) {
			const char* v = argv[i] + 7;
			bool phys = false;
			if (strncmp(v, "p:", 2) == 0) { phys = true; v += 2; }
			char* end;
			unsigned long a = strtoul(v, &end, 0);
			unsigned long len = (*end == ',') ? strtoul(end + 1, nullptr, 0) : 1;
			wlogs.push_back({ phys, a, len });
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
	auto tick       = std::make_shared<PicoTick>((uint32_t)tick_cycles);	// default 50Hz at ~1MHz; --tick to amplify IRQ-race pressure
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
	// CPU sees it (DAT-translated). Both forms are NON-PERTURBING:
	// read_physical and peek() add no guest cycles and trigger no device
	// side-effects, so observing a watch/dump can't detune the cycle-based
	// timer race that produces the U-067 crash. (Previously the logical
	// form went through cpu.read(), whose ++cycles advanced the timer and
	// made the bug vanish — the Heisenbug.)
	auto read_byte = [&](bool phys, unsigned long a) -> Byte {
		return phys ? datram->read_physical((size_t)a)
			    : datram->peek((Word)a);
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

	// --- Non-perturbing write-log + crash post-mortem (U-067) ----------
	// Register every --wlog range with DATRAM and install a callback that
	// records the store (cycle, storing-PC, target, old/new) and prints it
	// live. Pushing to a vector and an fprintf add NO guest cycles and no
	// device side-effects, so a deterministic timing-race crash survives
	// being watched — the whole point of doing this in the simulator.
	// Full last-writer shadow: for each physical byte, the PC + cycle of
	// the instruction that last wrote it. Lets the post-mortem name the
	// exact writer of the corrupt resume-PC slot regardless of which page
	// the run allocated. ~12MB; only allocated with --shadow.
	std::vector<Word>     shadow_pc;
	std::vector<uint32_t> shadow_cyc;
	if (track_shadow) {
		shadow_pc.assign(physical_ram_size, 0);
		shadow_cyc.assign(physical_ram_size, 0);
		datram->set_shadow_cb([&](size_t phys) {
			// Skip trampoline ($E400-$E4FF) writers: those are the
			// hardware IRQ-push bounces that re-stamp an already-corrupt
			// frame. We want the last REAL kernel writer of each byte.
			Word w = cpu.get_insn_pc();
			// (filter disabled for verification)
			if (phys < physical_ram_size) {
				shadow_pc[phys]  = w;
				shadow_cyc[phys] = (uint32_t)cpu.get_total_cycles();
			}
		});
	}

	std::vector<WlogRec> wlog_recs;
	// Host-side ring of the most-recently-executed instruction PCs. Dumped
	// by the crash post-mortem to expose the wild control transfer that
	// precedes a sled — deterministic regardless of guest timing. Recorded
	// in the step loop below (zero guest effect).
	const size_t PC_RING = 512;
	std::vector<Word> pc_ring(PC_RING, 0);
	size_t pc_ring_pos = 0;
	// Ring of control-flow DISCONTINUITIES (jumps/calls/returns): when an
	// instruction's PC isn't a fall-through from the previous one. A $00
	// sled is sequential, so the last discontinuity before a sled is the
	// wild transfer that started it — exactly what we need to find.
	const size_t JMP_RING = 64;
	std::vector<Word> jmp_from(JMP_RING, 0), jmp_to(JMP_RING, 0);
	size_t jmp_pos = 0;
	Word prev_pc = 0;
	Word min_ksp = 0xFFFF; Word min_ksp_pc = 0; unsigned long long min_ksp_cyc = 0;
	// Detector A persistence: a real double-alloc survives (victim sleeps); a
	// fork/exec setup transient resolves fast. Require the collision to persist.
	unsigned da_pg=0, da_e1=0, da_e2=0; unsigned long da_since=0; bool da_have=false;
	// Detector B state: protected saved-frame ranges of SLEEPING tasks, and
	// the hit reports. Ranges are PHYSICAL [lo,hi) (page<<13 | (usp&$1FFF)).
	struct Prot { size_t lo, hi; unsigned page, tslot; };
	std::vector<Prot> prot;
	bool stray_hit=false; Word stray_virt=0; size_t stray_phys=0;
	unsigned stray_page=0, stray_vtask=0; Word stray_pc=0;
	bool dat_hit=false; unsigned dat_slot=0; Byte dat_old=0, dat_new=0; Word dat_pc=0;
	if (detect_stray) {
		datram->set_store_cb([&](Word virt, size_t phys_or_idx, Byte val, bool is_dat) {
			if (is_dat) {
				// DAT-table write. Kernel bank-0 slots 0-4 (idx 0..4) map the
				// kernel text/data identity and must NEVER change after boot.
				unsigned idx = (unsigned)phys_or_idx;
				if (idx == 0 && val != 0 && !dat_hit) {
					dat_hit=true; dat_slot=idx;
					dat_old=datram->peek((Word)(0xFE00 + idx)); dat_new=val;
					dat_pc=cpu.get_insn_pc();
				}
				return;
			}
			if (stray_hit) return;
			for (const auto& p : prot)
				if (phys_or_idx >= p.lo && phys_or_idx < p.hi) {
					stray_hit=true; stray_virt=virt; stray_phys=phys_or_idx;
					stray_page=p.page; stray_vtask=p.tslot; stray_pc=cpu.get_insn_pc();
					return;
				}
		});
	}
	// U-067 Detector: a kernel store through the SBUF copy window ($C000-$DFFF)
	// whose translated physical target is the CURRENT task's user-block page
	// (slot 5) at an offset in the system-stack zone ($1F30-$1F40, just below
	// SYSSTK=$BF40). Fires atomically AT the corrupting write and dumps the
	// user stack pointer (usp @ $BF40, ust offset 0) and the I/O-target record
	// (uiosp/uistrt/uicnt @ ~$BFB0) so we can see whether the destination is a
	// genuine user-stack buffer (usp adjacent) or a stray pointer. peek() is
	// non-perturbing. One-shot-ish (capped) to avoid flooding.
	static int iostk_hits = 0;
	// U-067 (a)-vs-(b): per physical user-block page, remember the LAST
	// non-trampoline write to the usp field (ust offset 0 = page offset $1F40).
	// fixarg sets it through slot 5 ($BF40); dupum's fork copy sets it through
	// the SBUF window ($DF40) -- both land at physical offset $1F40, so we match
	// on the PHYSICAL offset to catch either. The trampoline (PC>=$E000) saves
	// the *running* S on every trap; we exclude it so this records only the
	// exec/fork LAUNCH placement. At the crash we print the crashing page's
	// launch usp + the PC that set it: a low value via fixarg => the process
	// started low and raised S in user code (b); a high value (or a dupum-copy
	// PC) => it was launched high via fork inheritance (a).
	static unsigned init_usp[256]   = {0};
	static unsigned init_usp_pc[256]= {0};
	static unsigned long long init_usp_cyc[256] = {0};
	static Byte     init_hi[256]    = {0};
	if (detect_iostk) {
		datram->set_store_cb([&](Word virt, size_t phys, Byte val,
					 bool is_dat) {
			if (is_dat) return;
			unsigned pc = cpu.get_insn_pc();
			// --- track launch placement of usp (phys offset $1F40/$1F41) ---
			if (pc < 0xE000) {
				unsigned poff = (unsigned)(phys & 0x1FFF);
				size_t pg = phys >> 13;
				if (pg < 256 && poff == 0x1F40) {
					init_hi[pg] = val;
				} else if (pg < 256 && poff == 0x1F41) {
					init_usp[pg]    = (init_hi[pg] << 8) | val;
					init_usp_pc[pg] = pc;
					init_usp_cyc[pg]= cpu.get_total_cycles();
				}
			}
			// --- the corrupting SBUF-window store into the stack zone ---
			if (virt < 0xC000 || virt > 0xDFFF) return;	// SBUF copy window
			Byte t = datram->get_task();
			Byte slot5 = datram->peek((Word)(0xFE00 + (t << 3) + 5));
			if ((phys >> 13) != (size_t)slot5) return;	// not the slot-5 page
			unsigned off = (unsigned)(phys & 0x1FFF);
			if (off < 0x1F30 || off > 0x1F40) return;	// stack-overlap zone
			if (iostk_hits++ >= 8) return;
			unsigned uview = 0xA000u | off;			// user-view address
			unsigned usp = (datram->peek(0xBF40) << 8) | datram->peek(0xBF41);
			fprintf(stderr,
			    "IOSTK#%d cyc=%llu PC=$%04X -> user-view $%04X (SBUF virt=$%04X "
			    "phys=$%05zX) val=$%02X | task=%u slot5_page=$%02X | "
			    "usp(@$BF40)=$%04X SYSSTK=$BF40 | LAUNCH usp=$%04X by PC=$%04X "
			    "cyc=%llu | ust $BFB0:",
			    iostk_hits, (unsigned long long)cpu.get_total_cycles(),
			    pc, uview, (unsigned)virt, phys,
			    (unsigned)val, (unsigned)t, (unsigned)slot5, usp,
			    init_usp[slot5], init_usp_pc[slot5],
			    (unsigned long long)init_usp_cyc[slot5]);
			for (Word a = 0xBFB0; a <= 0xBFB7; a++)
				fprintf(stderr, " %02X", (unsigned)datram->peek(a));
			fprintf(stderr, "\n");
		});
	}
	if (!wlogs.empty()) {
		for (const auto& w : wlogs)
			datram->add_wlog_range(w.phys, (size_t)w.addr, (size_t)w.len);
		datram->set_wlog_cb([&](Word virt, size_t phys, Byte oldv, Byte newv) {
			wlog_recs.push_back({ cpu.get_total_cycles(), cpu.get_insn_pc(),
					      virt, phys, oldv, newv });
			fprintf(stderr, "WLOG: cyc=%llu PC=$%04X virt=$%04X phys=$%05zX "
				"%02X->%02X\n",
				(unsigned long long)cpu.get_total_cycles(),
				(unsigned)cpu.get_insn_pc(), (unsigned)virt, phys,
				(unsigned)oldv, (unsigned)newv);
		});
	}

	// Automatic post-mortem on a fatal abort (e.g. the $BF3D invalid
	// instruction). Fires at the deterministic instant of death, reading
	// only host-side state (peek/read_physical) — zero perturbation.
	cpu.on_invalid = [&](const char* /*msg*/) {
		fprintf(stderr, "\n==== CRASH POST-MORTEM ====\n");
		fprintf(stderr, "PC=$%04X insn_pc=$%04X A=%02X B=%02X X=%04X Y=%04X "
			"U=%04X S=%04X CC=%02X DP=%02X cycles=%llu\n",
			(unsigned)cpu.get_pc(), (unsigned)cpu.get_insn_pc(),
			(unsigned)cpu.get_a(), (unsigned)cpu.get_b(),
			(unsigned)cpu.get_x(), (unsigned)cpu.get_y(),
			(unsigned)cpu.get_u(), (unsigned)cpu.get_s(),
			(unsigned)cpu.get_cc(), (unsigned)cpu.get_dp(),
			(unsigned long long)cpu.get_total_cycles());
		Byte t = datram->get_task();
		fprintf(stderr, "DAT task=%u slots:", (unsigned)t);
		for (int s = 0; s < 8; s++)
			fprintf(stderr, " %02X",
				(unsigned)datram->peek((Word)(0xFE00 + (t << 3) + s)));
		fprintf(stderr, "\n");
		fprintf(stderr, "user block $BF40-$BFF7 (current task %u):\n", (unsigned)t);
		for (Word a = 0xBF40; a <= 0xBFF0; a += 16) {
			fprintf(stderr, "  $%04X:", (unsigned)a);
			for (int i = 0; i < 16; i++)
				fprintf(stderr, " %02X", (unsigned)datram->peek((Word)(a + i)));
			fprintf(stderr, "\n");
		}
		// Slot-0 page (current bank) start: is the program text present or zero?
		fprintf(stderr, "slot-0 page $0000-$003F (current bank):\n");
		for (Word a = 0x0000; a <= 0x0030; a += 16) {
			fprintf(stderr, "  $%04X:", (unsigned)a);
			for (int i = 0; i < 16; i++)
				fprintf(stderr, " %02X", (unsigned)datram->peek((Word)(a + i)));
			fprintf(stderr, "\n");
		}
		// Who last wrote the slot-0 text bytes around the resume PC? If
		// it's the exec loader (ldfil/filrd) the page is the original; if
		// it's a copy/clear for another context, it's a use-after-free.
		if (track_shadow) {
			Word ipc = cpu.get_insn_pc();
			fprintf(stderr, "slot-0 last-writers around resume PC $%04X:\n",
				(unsigned)ipc);
			for (int off = -4; off <= 6; off++) {
				Word va = (Word)(ipc + off);
				size_t ph = datram->virt_to_phys(va);
				if (ph == (size_t)-1) continue;
				fprintf(stderr, "  virt $%04X phys $%05zX = %02X  "
					"last-writer PC=$%04X cyc=%u%s\n",
					(unsigned)va, ph, (unsigned)datram->peek(va),
					(unsigned)shadow_pc[ph], shadow_cyc[ph],
					off == 0 ? "  <-- PC" : "");
			}
		}
		// The saved-frame region around S (the rti popped its PC from here).
		Word sreg = cpu.get_s();
		Word fb = (sreg >= 0x20) ? (Word)(sreg - 0x20) : 0;
		fprintf(stderr, "stack frame $%04X-$%04X (S=$%04X):\n", fb, (unsigned)(fb+0x3F), sreg);
		for (Word a = fb; a <= (Word)(fb + 0x30); a += 16) {
			fprintf(stderr, "  $%04X:", (unsigned)a);
			for (int i = 0; i < 16; i++)
				fprintf(stderr, " %02X", (unsigned)datram->peek((Word)(a + i)));
			fprintf(stderr, "\n");
		}
		// Last-writer of the corrupt resume frame. The rti popped PC last,
		// so the PC slot is at [S-2 .. S-1]; dump the whole popped frame's
		// provenance ([S-12 .. S-1]) plus a couple bytes either side.
		if (track_shadow) {
			fprintf(stderr, "last-writer shadow around resume frame "
				"(S=$%04X, PC slot = S-2 = $%04X):\n",
				(unsigned)sreg, (unsigned)(Word)(sreg - 2));
			for (int off = -14; off <= 2; off++) {
				Word va = (Word)(sreg + off);
				size_t ph = datram->virt_to_phys(va);
				const char* tag = (off == -2) ? "  <-- PC hi"
						: (off == -1) ? "  <-- PC lo" : "";
				if (ph == (size_t)-1) {
					fprintf(stderr, "  virt $%04X: <unmapped>%s\n",
						(unsigned)va, tag);
				} else {
					fprintf(stderr, "  virt $%04X phys $%05zX = %02X  "
						"last-writer PC=$%04X cyc=%u%s\n",
						(unsigned)va, ph, (unsigned)datram->peek(va),
						(unsigned)shadow_pc[ph], shadow_cyc[ph], tag);
				}
			}
		}
		size_t n = wlog_recs.size();
		size_t start = (n > 64) ? n - 64 : 0;
		fprintf(stderr, "write-log: %zu entries (last %zu):\n", n, n - start);
		for (size_t k = start; k < n; k++) {
			const auto& r = wlog_recs[k];
			fprintf(stderr, "  cyc=%llu PC=$%04X virt=$%04X phys=$%05zX %02X->%02X\n",
				(unsigned long long)r.cyc, (unsigned)r.pc,
				(unsigned)r.virt, r.phys, (unsigned)r.oldv, (unsigned)r.newv);
		}
		fprintf(stderr, "recent executed PCs (oldest->newest, last %zu):\n",
			PC_RING);
		for (size_t j = 0; j < PC_RING; j++) {
			Word p = pc_ring[(pc_ring_pos + j) % PC_RING];
			fprintf(stderr, " %04X", (unsigned)p);
			if ((j & 15) == 15) fprintf(stderr, "\n");
		}
		fprintf(stderr, "\n");
		// Stale kernel-stack call-chain: SYSSTK=$BF40 grows down; after the
		// offending syscall returned, $BF40-down still holds its return-address
		// chain as stale data. Scan for words pointing into kernel text
		// ($4000-$8D17) -- those are the bsr/jsr return addresses of the path
		// that produced the corrupt frame. (Current bank slot 5 == this task's
		// user-block page == where the kernel stack lives.)
		fprintf(stderr, "stale kernel-stack call-chain ($BF3E down to $BD00, kernel-text words):\n");
		for (Word a = 0xBF3E; a >= 0xBD00 && a <= 0xBF3E; a -= 1) {
			Word w = (Word)((datram->peek(a) << 8) | datram->peek((Word)(a + 1)));
			if (w >= 0x4000 && w <= 0x8D17)
				fprintf(stderr, "  [$%04X] = $%04X\n", (unsigned)a, (unsigned)w);
		}
		fprintf(stderr, "recent control-flow jumps (from->to, oldest->newest):\n");
		for (size_t j = 0; j < JMP_RING; j++) {
			size_t k = (jmp_pos + j) % JMP_RING;
			if (jmp_from[k] == 0 && jmp_to[k] == 0) continue;
			fprintf(stderr, "  %04X -> %04X\n",
				(unsigned)jmp_from[k], (unsigned)jmp_to[k]);
		}
		do_dumps();
		fprintf(stderr, "kernel-stack low-water: min S=$%04X at PC=$%04X cyc=%llu (SYSSTK=$BF40)\n", (unsigned)min_ksp, (unsigned)min_ksp_pc,
			(unsigned long long)min_ksp_cyc);
		{
			auto rp=[&](unsigned a){ return (unsigned)datram->read_physical(a); };
			unsigned tb=(rp(0x50)<<8)|rp(0x51), te=(rp(0x52)<<8)|rp(0x53);
			unsigned ut=(datram->peek(0xBF5B)<<8)|datram->peek(0xBF5C);
			unsigned cur=(tb&&te>tb&&ut>=tb&&ut<te)?(ut-tb)/29:999;
			fprintf(stderr, "task table (cur=task#%u, utask=$%04X):\n", cur, ut);
			if (tb>=0x100 && te>tb && te<=0x3FFF && (te-tb)%29==0)
				for (unsigned e=tb,i=0; e<te; e+=29,i++)
					if (rp(e+5))
						fprintf(stderr, "  task#%u tsutop=$%02X tsstat=%u tsmode=$%02X%s\n",
							i, rp(e+4), rp(e+5), rp(e+6), i==cur?"  <-- current":""), fprintf(stderr, "      tstid=$%04X tstidp=$%04X\n", (rp(e+10)<<8)|rp(e+11), (rp(e+12)<<8)|rp(e+13));
		}
		fprintf(stderr, "==== END POST-MORTEM ====\n");
	};

	// Step loop is engaged when any of --timeout, --watch, --dump, --brk
	// are active. --brk-gated implies --brk semantics even without --brk
	// addresses, but on its own it's a no-op until the guest pokes
	// $FFCB and there are addresses to fire on.
	bool stepping = (timeout > 0) || !watches.empty()
		     || !dumps.empty() || !brk_addrs.empty()
		     || trace_from || trace_to || trace_procs;

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
			// U-080: optional ACTIVE-TASK filter. The current task's TCB
			// pointer is utask in the mapped user block ($BF5B/$BF5C) -- the
			// same source the U-067 detector uses. When --trace-task=<TCB> is
			// given, trace + brk fire only while that task is current (which
			// includes the kernel running on its behalf, until a context
			// switch rewrites utask). This cuts through the relasmb/kernel PC
			// overlap that otherwise makes single-task tracing ambiguous.
			bool task_ok = true;
			if (trace_task >= 0) {
				unsigned cur = (datram->peek(0xBF5B) << 8)
					     |  datram->peek(0xBF5C);
				task_ok = (cur == (unsigned)trace_task);
			}
			if (trace_task >= 0) {
				// Level-driven trace, gated by the active task. The window is
				// --trace-from/to if given, else whole-run --trace, else off
				// (so --trace-task with only --brk is a brk-only filter).
				bool win;
				if (trace_from || trace_to)
					win = (!trace_from || count >= trace_from)
					   && (!trace_to   || count <  trace_to);
				else
					win = trace;
				if (win && task_ok) cpu.tron(); else cpu.troff();
			} else {
				if (trace_from && count == trace_from) cpu.tron();
				if (trace_to && count == trace_to) cpu.troff();
			}
			if (ctrace_from) {
				unsigned long long cyc = cpu.get_total_cycles();
				if (cyc >= ctrace_from && (!ctrace_to || cyc < ctrace_to)) cpu.tron();
				else cpu.troff();
			}
			if (!brk_addrs.empty() && brk_enabled && task_ok) {
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
			// ---- U-080 process tracer: fork / exec / exit ----
			// Makes "which program is at which TCB" knowable over time (a TCB is
			// reused across fork/exec/exit, so trace-by-TCB alone is ambiguous).
			// Kernel hook PCs (text base = kernel-file-off + $3FE8):
			//   $789E fork  ldd utask/pshs d,y/lbsr lfork : D=parent TCB, Y=child slot
			//   $75F0 exec  (right after ldx BHDSIZ,s)     : X=exec'd fdn, fnumbr@X+5;
			//                                                utask($BF5B)=exec'ing TCB
			//   $79AA exit  sta tsstat,y (=TTERM)          : Y=exiting TCB
			// utask + the fdn are in page-0-identity / mapped-user-block RAM, so
			// datram->peek() (current bank) reads them while the kernel runs.
			if (trace_procs) {
				Word pcnow = cpu.get_pc();
				if (pcnow == 0x789E) {
					unsigned parent = ((unsigned)cpu.get_a() << 8) | cpu.get_b();
					unsigned child  = cpu.get_y();
					fprintf(stderr, "PROC fork parent=$%04X child=$%04X [insn#%lu]\n",
						parent, child, count);
				} else if (pcnow == 0x75F0) {
					unsigned fdn = cpu.get_x();
					unsigned fnumbr = ((unsigned)datram->peek((Word)(fdn + 5)) << 8)
							| datram->peek((Word)(fdn + 6));
					unsigned utaskp = ((unsigned)datram->peek(0xBF5B) << 8)
							| datram->peek(0xBF5C);
					fprintf(stderr, "PROC exec task=$%04X fnumbr=%u (fdn=$%04X) [insn#%lu]\n",
						utaskp, fnumbr, fdn, count);
				} else if (pcnow == 0x79AA) {
					fprintf(stderr, "PROC exit task=$%04X [insn#%lu]\n",
						(unsigned)cpu.get_y(), count);
				}
			}
			// ---- getpag/givpag live-page detector (U-067) ----
			// At getpag's return ($61FC, B=page from free list) and givpag's
			// entry ($6212, B=page to free), check B against every live task's
			// tsutop. A free/returned page must NOT be a live task's user-block
			// page. Reads PHYSICAL task table (page 0 identity).
			if (detect_free) {
				Word pcnow = cpu.get_pc();
				if (pcnow == 0x61FC || pcnow == 0x6212) {
					unsigned pg = cpu.get_b();
					if (pg >= 0x0B) {
						auto rb = [&](unsigned a){ return (unsigned)datram->read_physical(a); };
						unsigned base = (rb(0x50)<<8)|rb(0x51), end=(rb(0x52)<<8)|rb(0x53);
						// current task = utask ($BF5B) -> TCB addr -> index. Exclude
						// the current task (legit self-free during its own exec).
						unsigned utaskp = (datram->peek(0xBF5B)<<8)|datram->peek(0xBF5C);
						unsigned curidx = (utaskp>=base && utaskp<end) ? (utaskp-base)/29 : 999;
						if (base>=0x0100 && end>base && end<=0x3FFF && (end-base)%29==0) {
							for (unsigned e=base; e<end; e+=29) {
								unsigned st=rb(e+5);
								unsigned ti=(e-base)/29;
								if (rb(e+4)==pg && (st==1||st==2||st==3||rb(e+6)!=0)) {
									Word sp=cpu.get_s();
									unsigned caller=(datram->peek(sp)<<8)|datram->peek((Word)(sp+1));
									if (pcnow==0x6212 && ti!=curidx) {
										fprintf(stderr, "GIVPAG frees LIVE PAGE $%02X: task#%u stat=%u mode=$%02X "
											"caller=$%04X [insn#%lu]\n", pg, ti, st, rb(e+6), caller, count);
										if (cpu.on_invalid) cpu.on_invalid("givpag non-self live page");
										return 0;
									} else if (pcnow==0x61FC) {
										fprintf(stderr, "GETPAG=$%02X claimed by task#%u stat=%u mode=$%02X "
											"curidx=%u caller=$%04X [insn#%lu]\n", pg, ti, st, rb(e+6),
											curidx, caller, count);
									}
								}
							}
						}
					}
				}
			}
			cpu.tick();
			{
				Word ip = cpu.get_insn_pc();
				pc_ring[pc_ring_pos] = ip;
				pc_ring_pos = (pc_ring_pos + 1) % PC_RING;
				// Not a fall-through (allow up to 5-byte instructions)?
				bool disc = (ip < prev_pc || ip > (Word)(prev_pc + 5));
				if (disc) {
					jmp_from[jmp_pos] = prev_pc;
					jmp_to[jmp_pos]   = ip;
					jmp_pos = (jmp_pos + 1) % JMP_RING;
				}
				Word src_pc = prev_pc;
				// Spiral detector: trampoline trap entries ($E400 IRQ / $E42F
				// SWI3) coming <40 instrs apart, sustained, = the death-spiral
				// (process trapping constantly). Report the START (first dense
				// entry) so a trace window before it catches the first domino.
				if (trap_spin && (ip == 0xE400 || ip == 0xE42F)) {
					static unsigned long last_ent = 0, rapid = 0, start_cnt = 0;
					static unsigned long long start_cyc = 0;
					if (count - last_ent < 40) {
						if (rapid == 0) { start_cnt = count; start_cyc = cpu.get_total_cycles(); }
						rapid++;
					} else rapid = 0;
					last_ent = count;
					if (rapid >= 20) {
						fprintf(stderr, "SPIRAL-START: ~insn#%lu cyc=%llu (20 dense entries)\n",
							start_cnt, (unsigned long long)start_cyc);
						if (cpu.on_invalid) cpu.on_invalid("SPIRAL DETECTED");
						return 0;
					}
				}
				prev_pc = ip;
				// U-067: catch the instruction where USER-mode S first rises
				// into the system-stack zone [$BE00,$BF40) -- the instruction
				// that parks the Pascal stack on top of SYSSTK. Logs insn_pc
				// (the user-program PC that just raised S), the S transition,
				// and the task. Transition-only (prev<$BE00 -> now in zone) so
				// it fires at the raise, not on every subsequent high store.
				if (detect_srise) {
					// Per-task: the upward transition where S jumps from below
					// the designed top ($BE80) into the SYSSTK danger vicinity
					// [$BF20,$BF40). insn_pc is the instruction that raised S
					// (the Pascal stack being parked on top of SYSSTK).
					static Word srise_prev[32] = {0};
					static int srise_hits = 0;
					Byte tk = datram->get_task() & 31;
					Word ipx = cpu.get_insn_pc();
					Word sp  = cpu.get_s();
					bool kernel_text = (ipx >= 0x4000 && ipx <= 0x8D17);
					if (!kernel_text && srise_prev[tk]
					    && srise_prev[tk] < 0xBE80
					    && sp >= 0xBF20 && sp < 0xBF40 && srise_hits < 24) {
						srise_hits++;
						fprintf(stderr, "SRISE#%d insn_pc=$%04X S:$%04X->$%04X "
							"TRUSER=$%02X task=%u cyc=%llu\n", srise_hits,
							(unsigned)ipx, (unsigned)srise_prev[tk], (unsigned)sp,
							(unsigned)datram->peek(0xFDFA), (unsigned)tk,
							(unsigned long long)cpu.get_total_cycles());
					}
					srise_prev[tk] = sp;
				}
				// U-070: catch the FIRST time S is corrupted into high memory
				// ($FF00+ -- the $FFC0-$FFFF I/O+vector zone). A stack pointer
				// can never legitimately live there. Logs the instruction that
				// produced it, the S transition, TRUSER, umapno, bank and cycle
				// so the fast-tick S->$FFFx root is pin-pointable. Per-task,
				// transition-only (prev<$FF00 -> now >=$FF00).
				if (detect_sphi) {
					static Word sphi_prev[32] = {0};
					static int sphi_hits = 0;
					Byte tk = datram->get_task() & 31;
					Word ipx = cpu.get_insn_pc();
					Word sp  = cpu.get_s();
					if (sphi_prev[tk] < 0xFF00 && sp >= 0xFF00 && sphi_hits < 24) {
						sphi_hits++;
						fprintf(stderr, "SPHI#%d insn_pc=$%04X S:$%04X->$%04X "
							"TRUSER=$%02X umapno=$%02X task=%u cyc=%llu\n", sphi_hits,
							(unsigned)ipx, (unsigned)sphi_prev[tk], (unsigned)sp,
							(unsigned)datram->peek(0xFDFA), (unsigned)datram->peek(0xBF43),
							(unsigned)tk, (unsigned long long)cpu.get_total_cycles());
					}
					sphi_prev[tk] = sp;
				}
				// Kernel-stack low-water: track min S while in kernel context
				// (TRUSER @ $FDFA == 0). If it descends into the user frames,
				// the kernel stack is overflowing into saved user state.
				if (datram->peek(0xFDFA) == 0) {
					Word sp = cpu.get_s();
					// plausible kernel-stack range only (skip boot S=0 and spiral wild-lows)
					// only while executing REAL kernel text ($4000-$8D17); skip the
					// trampoline ($E0xx) where S is still the user stack pre-switch.
					if (sp >= 0xA000 && sp < 0xBF40 && ip >= 0x4000 && ip <= 0x8D17
					    && cpu.get_total_cycles() > 300000
					    && sp < min_ksp) { min_ksp = sp; min_ksp_pc = ip; min_ksp_cyc = cpu.get_total_cycles(); }
				}
				// First wild transfer onto a $00 sled: a control-flow
				// discontinuity landing on a RUN of $00 bytes (a real
				// NEG-<$00 sled, not a lone NEG <$01 = $00 $01 from a
				// legit syscall return). 12+ consecutive zeros is never
				// real code. Catches the originating wild jump ~30K insns
				// before the final invalid opcode.
				// FIRST bad kernel vector dispatch (spiral-proof first domino):
				// a jsr [irqvec]/[sw3vec] in the trampoline ($E41F/$E42A ->
				// irqhan $88AB; $E449 -> swi3han $890B) resolving to anything
				// else means slot 0 of the current bank is not kernel page 0.
				if (trap_sled && disc
				    && (src_pc == 0xE41F || src_pc == 0xE42A || src_pc == 0xE449)
				    && datram->get_task() != 0) {  // bad only if dispatched in a USER bank
					fprintf(stderr, "BAD-DISPATCH: src=$%04X -> $%04X "
						"(TASKREG/task=%u)\n", (unsigned)src_pc, (unsigned)ip,
						(unsigned)datram->get_task());
					if (cpu.on_invalid)
						cpu.on_invalid("BAD VECTOR DISPATCH (first domino)");
					return 0;
				}
				// FIRST DOMINO: a user-bank process (get_task()!=0) entering
				// the fixed page ($E000-$FDFF) via a jump/rti at a NON-vector
				// address. Legit trap entries are $E400/$E42F/$E450/$E451; a
				// jump/rti to anywhere else in the fixed page with a user bank
				// mapped means corrupt control flow JUST reached the trampoline.
				// U-069 added the task-1 bootstrap INTO the fixed page: strtup
				// jmps to tr_uent ($E4AC) which rti's to ts1cnt ($E4B0), both run
				// as user code in the user bank during init -- legit, exclude them
				// (plus tr_irqk $E426, the kernel-context IRQ entry).
				if (trap_sled && disc && datram->get_task() != 0
				    && ip >= 0xE000 && ip <= 0xFDFF
				    && ip != 0xE400 && ip != 0xE42F && ip != 0xE450 && ip != 0xE451
				    && ip != 0xE426 && ip != 0xE4AC && ip != 0xE4B0) {
					fprintf(stderr, "FIRST-DOMINO: $%04X -> $%04X (task=%u)\n",
						(unsigned)src_pc, (unsigned)ip, (unsigned)datram->get_task());
					if (cpu.on_invalid) cpu.on_invalid("USER ENTERED TRAMPOLINE (first domino)");
					return 0;
				}
				// Spiral entry: tr_exit's rti ($E475) returning a process
				// into the FIXED page ($E000-$FDFF) -- a "user return" into
				// the trampoline, run as user code (the $E4xx death-spiral
				// the $00-sled trap misses).
				// Exclude the 5 legit trap-entry points ($E400 IRQ, $E426
				// tr_irqk, $E42F SWI3, $E450 FIRQ, $E451 SWI2) -- a return
				// landing on those is a normal IRQ-at-rti, not a spiral.
				if (trap_sled && disc && src_pc == 0xE475
				    && ip >= 0xE000 && ip <= 0xFDFF
				    && ip != 0xE400 && ip != 0xE426 && ip != 0xE42F
				    && ip != 0xE450 && ip != 0xE451) {
					if (cpu.on_invalid)
						cpu.on_invalid("SPIRAL ENTRY (tr_exit rti -> fixed page)");
					return 0;
				}
				if (trap_sled && disc && datram->peek(ip) == 0x00) {
					bool sled = true;
					for (int z = 1; z < 12; z++)
						if (datram->peek((Word)(ip + z)) != 0x00) { sled = false; break; }
					if (sled) {
						if (cpu.on_invalid)
							cpu.on_invalid("SLED ENTRY (first wild transfer)");
						return 0;
					}
				}
			}
			// ---- U-067 Detector A: page double-allocation ----
			// Task table: tsktab ptr @phys $0050, tskend @$0052; TSKSIZ=29,
			// tsutop@+4 = physical user-block page, tsstat@+5 (0=free). Two
			// LIVE tasks owning the same physical page = getpag double-alloc.
			// Reads are PHYSICAL (kernel page 0, identity) = true ownership.
			if (detect_dbl && (count & 63) == 0) {
				auto rb = [&](unsigned a){ return (unsigned)datram->read_physical(a); };
				unsigned base = (rb(0x50) << 8) | rb(0x51);
				unsigned end  = (rb(0x52) << 8) | rb(0x53);
				if (base >= 0x0100 && end > base && end <= 0x3FFF
				    && (end - base) % 29 == 0 && (end - base) / 29 <= 64) {
					for (unsigned e1 = base; e1 < end; e1 += 29) {
						unsigned st1 = rb(e1 + 5);
						if (st1 == 0) continue;
						unsigned pg1 = rb(e1 + 4);
						// Skip system pages ($00-$0A: SYSPAG/STABPG/SYSTXT/DRVPAG/text);
						// a real user-block page is >= $0B (first user-RAM page). A
						// $05 "collision" is just the no-user-block sentinel.
						if (pg1 < 0x0B) continue;
						for (unsigned e2 = e1 + 29; e2 < end; e2 += 29) {
							unsigned st2 = rb(e2 + 5);
							if (st2 == 0) continue;
							// Real bug: a SLEEPING victim (TSLEEP=2/TWAIT=3) shares its
							// user-block page with another live task. Fork/exec copy
							// transients are between running tasks -> excluded.
							if (rb(e2 + 4) == pg1 && (st1==2||st1==3||st2==2||st2==3)) {
								// candidate collision; trap only if it persists (>20000
								// insns) to filter fork/exec setup transients.
								if (da_have && da_pg==pg1 && da_e1==e1 && da_e2==e2) {
									if (count - da_since > 128) {
										fprintf(stderr, "DOUBLE-ALLOC (persisted %lu insns): "
											"task@$%04X (stat=%u) & task@$%04X (stat=%u) both "
											"own user-block page $%02X; first seen insn#%lu\n",
											count - da_since, e1, st1, e2, rb(e2 + 5), pg1, da_since);
										if (cpu.on_invalid) cpu.on_invalid("PAGE DOUBLE-ALLOC");
										return 0;
									}
								} else {
									da_have=true; da_pg=pg1; da_e1=e1; da_e2=e2; da_since=count;
								}
								goto da_done;
							}
						}
					}
				}
			}
			da_done:;
			// ---- Detector B: rebuild sleeping-task frame ranges + check hits ----
			if (detect_stray) {
				if ((count & 255) == 0) {
					prot.clear();
					auto rb = [&](unsigned a){ return (unsigned)datram->read_physical(a); };
					unsigned base = (rb(0x50) << 8) | rb(0x51);
					unsigned end  = (rb(0x52) << 8) | rb(0x53);
					if (base >= 0x0100 && end > base && end <= 0x3FFF
					    && (end - base) % 29 == 0 && (end - base) / 29 <= 64) {
						unsigned curpg = datram->peek(0xFE05);	// current user page (bank-0 slot 5)
						for (unsigned e = base; e < end; e += 29) {
							unsigned st = rb(e + 5);
							if (st != 1 && st != 2 && st != 3) continue;	// TRUN/TSLEEP/TWAIT only
							unsigned page = rb(e + 4);
							if (page < 0x0B) continue;		// skip system pages
							if (page == curpg) continue;		// skip the CURRENT task (legit self-writes)
							unsigned tslot = (e - base) / 29;
							// protect the per-task control block ust ($BF40-$BFF7)
							prot.push_back({ ((size_t)page<<13)|0x1F40, ((size_t)page<<13)|0x1FF8, page, tslot });
							// and the saved rti frame at usp (top word of ust)
							size_t ub = ((size_t)page << 13) | 0x1F40;
							unsigned usp = (rb(ub) << 8) | rb(ub + 1);
							if (usp >= 0xA000 && usp <= 0xBFFF) {
								size_t flo = ((size_t)page<<13) | (usp & 0x1FFF);
								prot.push_back({ flo, flo + 14, page, tslot });
							}
						}
					}
				}
				if (stray_hit) {
					fprintf(stderr, "STRAY-WRITE: PC=$%04X stored virt=$%04X -> "
						"phys=$%05zX, inside SLEEPING task#%u's saved frame "
						"(user page $%02X) [insn#%lu]\n", (unsigned)stray_pc,
						(unsigned)stray_virt, stray_phys, stray_vtask, stray_page, count);
					if (cpu.on_invalid) cpu.on_invalid("STRAY WRITE into sleeping frame");
					return 0;
				}
				if (dat_hit) {
					fprintf(stderr, "STRAY-DAT: PC=$%04X wrote kernel bank-0 DAT slot %u "
						"$%02X->$%02X (kernel mapping corruption) [insn#%lu]\n",
						(unsigned)dat_pc, dat_slot, (unsigned)dat_old, (unsigned)dat_new, count);
					if (cpu.on_invalid) cpu.on_invalid("STRAY DAT WRITE");
					return 0;
				}
			}
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
		// U-067: report the genuine kernel-stack low-water at clean exit
		// (depth = SYSSTK $BF40 - min S). Sizes the relocated kernel stack.
		if (min_ksp != 0xFFFF)
			fprintf(stderr, "kernel-stack low-water (clean exit): min S=$%04X "
				"depth=%u at PC=$%04X cyc=%llu\n", (unsigned)min_ksp,
				(unsigned)(0xBF40 - min_ksp), (unsigned)min_ksp_pc,
				(unsigned long long)min_ksp_cyc);
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
