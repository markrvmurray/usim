//
//	memory.h
//	(C) R.P.Bellis 2021 - 2025
//
//	vim: ts=8
//

#pragma once

#include <functional>
#include <vector>

#include "device.h"
#include "wiring.h"

/*
 * generic memory interface, dynamically assigned space
 */
class GenericMemory : public MappedDevice {

protected:
	std::vector<Byte>	memory;
	size_t			size;

public:
				GenericMemory(size_t size) : memory(size), size(size) {};

public:
	virtual Byte		read(Word offset) {
					return (offset < size) ? memory[offset] : 0xff;
				};

	virtual void		write(Word offset, Byte val) {
					if (offset < size) {
						memory[offset] = val;
					}
				};

	// Load an Intel HEX file, dropping records outside [base, base+size).
	// Lives on GenericMemory so both RAM and ROM can call it: a single
	// HEX file containing records for two address ranges (e.g. picolibc
	// no-flash binaries with .text in RAM and .init in ROM) is loaded by
	// invoking this on each device with that device's base.
	void			load_intelhex(const char *filename, Word base);

	// Same semantics as load_intelhex but for Motorola S-record files.
	// Recognises S1 (data) and S9 (end) record types; other types are
	// silently ignored.
	void			load_srec(const char *filename, Word base);

	// Same semantics as the above for ELF32 (the natural output of
	// llvm-mc6809's lld). Walks PT_LOAD program headers; each segment
	// with FileSiz > 0 is copied to memory[p_vaddr - base], silently
	// dropping any segment that falls outside this device's mapped
	// range. Both ELFDATA2MSB and ELFDATA2LSB are accepted.
	//
	// After the segment copy, two vector slots get symbol-table
	// fallbacks (a single symtab walk resolves both):
	//
	//   start_vector_addr (default $FFFE — reset) ← `_start`
	//   swi3_vector_addr  (default $FFF2 — SWI3)  ← `__swi3_trap`
	//
	// Each fallback fires only when its vector lies in this device's
	// range AND is still zero after the segment copy (no PT_LOAD
	// covered it). Pass either address as 0 to disable that fallback.
	//
	// The reset-vector fallback matches the MAME llvm6309 driver and
	// lets picolibc/llvm-mc6809 builds without a `.vectors` PT_LOAD
	// run unmodified. The SWI3 fallback supports llvm-mc6809's Bug
	// #273 diagnostic sentinel: if `__swi3_trap` is in the ELF symbol
	// table, an unexpected SWI3 executes through a known handler that
	// writes a distinctive sentinel to the halt port at $FFD2 rather
	// than dispatching to a wild PC.
	void			load_elf(const char *filename, Word base,
					 Word start_vector_addr = 0xFFFE,
					 Word swi3_vector_addr  = 0xFFF2);
};

/*
 * RAM: can be written to
 */
class RAM : public GenericMemory {

public:
				RAM(size_t size) : GenericMemory(size) {};

};

/*
 * DATRAM: paged RAM with a Dynamic Address Translator.
 *
 * The guest address space below `size` is split into two zones:
 *
 *   - Translated zone [0, fixed_start): mapped through the DAT page
 *     table. Each 8KB guest page consults one entry from the active
 *     task's row; the entry's value indexes an 8KB physical page in
 *     the backing store (`totalsize` bytes, typically 2MB).
 *   - Fixed zone [fixed_start, size): never DAT-translated. Maps
 *     directly to memory[fixed_phys_base + (offset - fixed_start)].
 *     `fixed_size = size - fixed_start`. When no fixed window has
 *     been configured (default), `fixed_size == 0` and the entire
 *     range [0, size) is translated.
 *
 * The DAT page table is exposed at offsets [size, size + datsize)
 * so the guest can read/write it directly (typically $FE00-$FEFF).
 *
 * The active task is selected via `set_task` (5 bits, 0-31), wired up
 * by the PicoTask register at $FFC0.
 *
 * Direct physical access (write_physical / load_physical) bypasses
 * translation and is used by the host to populate the backing store
 * before the guest starts running.
 *
 * Pico-thing logical map (the call site in main_picothing.cpp):
 *   $0000-$DFFF   translated (7 × 8KB pages via DAT)
 *   $E000-$FDFF   fixed → physical $1E000-$1FDFF (never remapped)
 *   $FE00-$FEFF   DAT page table
 * The board's never-remapped block is hardware-fixed; this class
 * models it via the fixed zone so the guest sees the same address
 * map as real pico-thing.
 *
 * "Page unavailable" sentinel: a DAT entry value of $FF marks the
 * page as unavailable — any guest read or write through that entry
 * asserts NMI (reads return $FF, writes are dropped). Identity
 * initialisation puts $FF in DAT slot index $FF (= $FEFF, task 31's
 * eighth entry), which for pico-thing is in the unused-page-7 column
 * — the fixed zone covers guest page 7, so that slot is never
 * actually consulted. The sentinel still applies in slots 0-6 of
 * every task.
 *
 * NMI is falling-edge sensitive on the 6809, so the assertion is
 * pulsed (held for one tick after the bad access, then released) to
 * allow subsequent bad accesses to re-trigger. The pulse state is
 * advanced by the DATRAMTicker adapter — DATRAM itself stays a plain
 * MappedDevice via GenericMemory to avoid a diamond through
 * MappedDevice.
 */
class DATRAM : public GenericMemory {

	size_t size, datsize;
	std::vector<Byte>	datram;
	Byte			task;

	// Fixed never-remapped window inside [0, size). When fixed_size==0
	// (default), no fixed window — the whole range is translated.
	size_t			fixed_start     = 0;
	size_t			fixed_size      = 0;
	size_t			fixed_phys_base = 0;

	// Active-low NMI pulse state.
	bool			m_nmi_request   = false;
	bool			m_clear_pending = false;

	// Non-perturbing write-log state (see public add_wlog_range/peek).
	struct WLogRange { bool phys; size_t lo, hi; };
	std::vector<WLogRange>	m_wlog;
	std::function<void(Word, size_t, Byte, Byte)> m_wlog_cb;
	std::function<void(size_t)> m_shadow_cb;	// full last-writer shadow
	// Per-store observer: (virtual offset, physical addr OR DAT index,
	// value, is_dat). Lets the host catch stray writes into other tasks'
	// pages and DAT-table corruption. is_dat=true => the 2nd arg is the
	// DAT slot index (0-255), not a backing-store address.
	std::function<void(Word, size_t, Byte, bool)> m_store_cb;

	// Report a pending store at (virt, phys) if it falls in a watched
	// range. Reads only host-side state; never touches the cycle counter
	// or any device. No-op (one empty-vector test) when no range is set.
	void			wlog_check(Word virt, size_t phys, Byte newv) const {
					if (m_wlog.empty() || !m_wlog_cb) return;
					for (const auto& r : m_wlog) {
						size_t key = r.phys ? phys : (size_t)virt;
						if (key >= r.lo && key < r.hi) {
							Byte oldv = (phys < memory.size())
								  ? memory[phys] : (Byte)0xFFu;
							m_wlog_cb(virt, phys, oldv, newv);
							return;
						}
					}
				}

	Byte			dat_entry(Word offset) const {
					return datram[(Byte)(task << 3) | (Byte)(offset >> 13)];
				}
	size_t			ext_offset(Word offset) {
					return ((size_t)datram[(Byte)(task << 3) | (Byte)(offset >> 13)] << 13) | (offset & 0x1FFF);
				}
	bool			in_fixed_window(Word offset) const {
					return fixed_size > 0
					    && offset >= fixed_start
					    && offset < fixed_start + fixed_size;
				}

public:
	// m_nmi_request=true → pin reads false (active-low asserted).
	OutputPin		NMI;

				DATRAM(size_t size, size_t totalsize, size_t datsize)
					 : GenericMemory(totalsize), size(size), datsize(datsize),
					   datram(datsize), task(0),
					   NMI(m_nmi_request, /*invert=*/true) {
						for (unsigned i = 0; i < datsize; i++) datram[i] = i;
					 }

	// Configure the fixed never-remapped window. Call once after
	// construction (before reset/run). `fixed_start + fixed_size`
	// must equal `size` — the fixed window directly precedes the
	// DAT page table.
	void			set_fixed_window(size_t start, size_t window_size, size_t phys_base) {
					fixed_start     = start;
					fixed_size      = window_size;
					fixed_phys_base = phys_base;
				}

	void			set_task(Byte t) { task = t & 0x1F; }
	Byte			get_task() const { return task; }

	// Direct physical memory access (bypasses DAT translation).
	void			write_physical(size_t addr, Byte val) {
					if (addr < memory.size()) memory[addr] = val;
				}
	Byte			read_physical(size_t addr) const {
					return (addr < memory.size()) ? memory[addr] : 0xFF;
				}
	void			load_physical(const uint8_t* data, size_t len, size_t offset) {
					for (size_t i = 0; i < len && offset + i < memory.size(); i++)
						memory[offset + i] = data[i];
				}

	// --- Non-perturbing write-log (U-067 Heisenbug breaker) -------------
	// A guest store whose target falls in a registered range is reported
	// through m_wlog_cb BEFORE the store lands (so the callback can read
	// the old byte). This path adds NO guest cycles and triggers NO device
	// side-effects, so the deterministic crash is preserved while watched.
	void			add_wlog_range(bool phys, size_t base, size_t len) {
					m_wlog.push_back({ phys, base, base + (len ? len : 1) });
				}
	// Callback args: (virtual offset, physical addr, old byte, new byte).
	void			set_wlog_cb(std::function<void(Word, size_t, Byte, Byte)> cb) {
					m_wlog_cb = std::move(cb);
				}

	// --- Full last-writer shadow (debug dirty trick, U-067) -------------
	// Called on EVERY guest store with the resolved physical address so the
	// host can record who wrote each byte. Lets a post-mortem ask "which
	// instruction last wrote this address?" -- robust to per-run page
	// allocation variance (no fixed --wlog address needed).
	void			set_shadow_cb(std::function<void(size_t)> cb) {
					m_shadow_cb = std::move(cb);
				}
	void			set_store_cb(std::function<void(Word, size_t, Byte, bool)> cb) {
					m_store_cb = std::move(cb);
				}

	// Translate a CPU (logical) address to its physical backing address
	// under the CURRENT task's DAT. Returns (size_t)-1 if unmapped/out of
	// the translated+fixed range. Pure (no side-effects).
	size_t			virt_to_phys(Word offset) const {
					if (offset < size) {
						if (in_fixed_window(offset))
							return fixed_phys_base + (offset - fixed_start);
						Byte e = datram[(Byte)(task << 3) | (Byte)(offset >> 13)];
						if (e == 0xFF) return (size_t)-1;
						return ((size_t)e << 13) | (offset & 0x1FFF);
					}
					return (size_t)-1;
				}

	// DAT-translating read that is PURE: no cycle increment, no NMI
	// side-effect. For host-side observation only (--watch/--dump/crash
	// dump) so watching can't detune the cycle-based timer race.
	Byte			peek(Word offset) const {
					if (offset < size) {
						if (in_fixed_window(offset)) {
							size_t phys = fixed_phys_base + (offset - fixed_start);
							return (phys < memory.size()) ? memory[phys] : (Byte)0xFFu;
						}
						Byte e = datram[(Byte)(task << 3) | (Byte)(offset >> 13)];
						if (e == 0xFF) return (Byte)0xFFu;
						size_t phys = ((size_t)e << 13) | (offset & 0x1FFF);
						return (phys < memory.size()) ? memory[phys] : (Byte)0xFFu;
					} else if (offset < size + datsize) {
						return datram[offset - size];
					}
					return (Byte)0xFFu;
				}

	// Driven by DATRAMTicker. Pulse: tick(N) where the bad access
	// landed sees m_nmi_request=true and arms m_clear_pending; tick(N+1)
	// releases the line. The CPU samples NMI between active-device
	// ticks and instruction execute, so the assertion is visible for
	// exactly one sample before release — long enough to edge-trigger
	// once, short enough to allow the next bad access to re-trigger.
	void			on_tick() {
					if (m_clear_pending) {
						m_nmi_request = false;
						m_clear_pending = false;
					} else if (m_nmi_request) {
						m_clear_pending = true;
					}
				}
	void			on_reset() {
					m_nmi_request = false;
					m_clear_pending = false;
				}

	virtual Byte		read(Word offset) {
					if (offset < size) {
						if (in_fixed_window(offset)) {
							size_t phys = fixed_phys_base + (offset - fixed_start);
							return (phys < memory.size()) ? memory[phys] : (Byte)0xFFu;
						}
						if (dat_entry(offset) == 0xFF) {
							m_nmi_request = true;
							return (Byte)0xFFu;
						}
						return memory[ext_offset(offset)];
					} else if (offset < size + datsize) {
						return datram[offset - size];
					} else {
						return (Byte)0xFFu;
					}
				}

	virtual void		write(Word offset, Byte val) {
					if (offset < size) {
						if (in_fixed_window(offset)) {
							size_t phys = fixed_phys_base + (offset - fixed_start);
							wlog_check(offset, phys, val);
							if (m_shadow_cb) m_shadow_cb(phys);
							if (m_store_cb) m_store_cb(offset, phys, val, false);
							if (phys < memory.size()) memory[phys] = val;
							return;
						}
						if (dat_entry(offset) == 0xFF) {
							m_nmi_request = true;
							return;
						}
						size_t phys = ext_offset(offset);
						wlog_check(offset, phys, val);
						if (m_shadow_cb) m_shadow_cb(phys);
						if (m_store_cb) m_store_cb(offset, phys, val, false);
						memory[phys] = val;
					} else if (offset < size + datsize) {
						// DAT page-table store ($FE00-$FEFF). No backing-
						// store phys; use a high pseudo-phys (> real 2MB) so
						// p:-watches don't match but a virtual watch on
						// $FE00+slot does — lets us catch DAT corruption.
						wlog_check(offset, (size_t)0x1000000u + (offset - size), val);
						if (m_store_cb) m_store_cb(offset, offset - size, val, true);
						datram[offset - size] = val;
					}
				}
};

/*
 * DATRAMTicker: ActiveDevice adapter that lets DATRAM participate in
 * the CPU's active-device tick (for its NMI pulse) and reset chain.
 * DATRAM itself is a MappedDevice via GenericMemory; routing
 * tick/reset through this adapter avoids making MappedDevice a virtual
 * base of GenericMemory.
 */
class DATRAMTicker : public ActiveDevice {
	DATRAM&			d;
public:
				DATRAMTicker(DATRAM& d) : d(d) {}
	void			tick(uint8_t /*cycles*/) override { d.on_tick(); }
	void			reset() override { d.on_reset(); }
};

/*
 * ROM: can't be written
 *      can be pre-loaded from a hex file
 */
class ROM : public GenericMemory {

public:
				ROM(size_t size) : GenericMemory(size) {};

	virtual void		write(Word offset, Byte val) {		// no-op
					(void)offset;
					(void)val;
				}
};

/*
 * ROM_Data
 *
 * For use when embedding static firmware data inside a compiled image
 */
class ROM_Data : public MappedDevice {

protected:
	const uint8_t*		memory;
	size_t			size;					// size of virtual device
	size_t			memsize;				// size of allocated data

public:
				ROM_Data(const uint8_t* p, size_t size) : memory(p), size(size), memsize(size) {};

				ROM_Data(const uint8_t* p, size_t size, size_t memsize) : memory(p), size(size), memsize(memsize) {};

	virtual Byte		read(Word offset) {
					if (offset < size && offset < memsize) {
						return memory[offset];
					} else {
						return 0xff;
					}
				}

	virtual void		write(Word offset, Byte val) {		// no-op
					(void)offset;
					(void)val;
				}
};
