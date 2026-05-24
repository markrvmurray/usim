//
//	memory.h
//	(C) R.P.Bellis 2021 - 2025
//
//	vim: ts=8
//

#pragma once

#include "device.h"

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
	// If the byte at the absolute address `start_vector_addr` (default
	// $FFFE — the 6809 reset vector) lies in this device's range and
	// is still zero after the segment copy, falls back to scanning the
	// symbol table for `_start` and writing that address there as a
	// big-endian word. Matches the MAME llvm6309 driver's behaviour and
	// lets picolibc/llvm-mc6809 builds without a `.vectors` PT_LOAD run
	// unmodified. Pass `start_vector_addr = 0` to disable the fallback.
	void			load_elf(const char *filename, Word base,
					 Word start_vector_addr = 0xFFFE);
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
 * The 64K guest address space below `size` (typically $0000-$FDFF) is
 * divided into 8KB pages. The DAT page table holds `datsize` entries
 * (256 bytes — 32 tasks * 8 pages); the currently-selected task picks
 * 8 entries, each mapping a guest 8KB window to one of up to 256
 * physical 8KB pages out of `totalsize` (typically 2MB).
 *
 * The page table itself is exposed at offsets [size, size + datsize)
 * so the guest can read/write it directly (typically $FE00-$FEFF).
 *
 * The active task is selected via `set_task` (5 bits, 0-31), wired up
 * by the PicoTask register at $FFC0.
 *
 * Direct physical access (write_physical / load_physical) bypasses
 * translation and is used by the host to populate the backing store
 * before the guest starts running.
 */
class DATRAM : public GenericMemory {

	size_t size, datsize;
	std::vector<Byte>	datram;
	Byte			task;

	size_t			ext_offset(Word offset) {
					return ((size_t)datram[(Byte)(task << 3) | (Byte)(offset >> 13)] << 13) | (offset & 0x1FFF);
				}

public:
				DATRAM(size_t size, size_t totalsize, size_t datsize)
					 : GenericMemory(totalsize), size(size), datsize(datsize), datram(datsize), task(0) {
						for (unsigned i = 0; i < datsize; i++) datram[i] = i;
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

	virtual Byte		read(Word offset) {
					if (offset < size) {
						return memory[ext_offset(offset)];
					} else if (offset < size + datsize) {
						return datram[offset - size];
					} else {
						return (Byte)0xFFu;
					}
				}

	virtual void		write(Word offset, Byte val) {
					if (offset < size) {
						memory[ext_offset(offset)] = val;
					} else if (offset < size + datsize) {
						datram[offset - size] = val;
					}
				}
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
