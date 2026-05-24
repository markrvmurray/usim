//
//	memory.c
//	(C) R.P.Bellis 2021 - 2025
//
//      vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "memory.h"

Byte fread_hex_byte(FILE *fp)
{
	char			str[3];
	long			l;

	str[0] = fgetc(fp);
	str[1] = fgetc(fp);
	str[2] = '\0';

	l = strtol(str, NULL, 16);
	return (Byte)(l & 0xff);
}

Word fread_hex_word(FILE *fp)
{
	Word		ret;

	ret = fread_hex_byte(fp);
	ret <<= 8;
	ret |= fread_hex_byte(fp);

	return ret;
}

void GenericMemory::load_intelhex(const char *filename, Word base)
{
	FILE		*fp;
	int		done = 0;

	fp = fopen(filename, "r");
	if (!fp) {
		perror("filename");
		exit(EXIT_FAILURE);
	}

	while (!done && !feof(fp)) {
		Byte		n, t;
		Word		addr;
		Byte		b;

		(void)fgetc(fp);		// colon
		n = fread_hex_byte(fp);		// byte count
		addr = fread_hex_word(fp);	// start memory address
		t = fread_hex_byte(fp);		// record type

		while (n--) {
			b = fread_hex_byte(fp);	// data byte
			if (t == 0x00) {
				if ((addr >= base) && (addr < ((DWord)base + size))) {
					memory[addr - base] = b;
				}
				++addr;
			} else if (t == 0x01) {
				done = 1;
			}
		}

		// Read and discard checksum byte
		(void)fread_hex_byte(fp);
		if (fgetc(fp) == '\r') (void)fgetc(fp);
	}
	fclose(fp);
}

static unsigned
hex(const char *buf, const unsigned count)
{
	unsigned val = 0u;
	unsigned tally = 0u;
	for (;;) {
		val <<= 4;
		if (*buf >= '0' && *buf <= '9')
			val += *buf - '0';
		else if (*buf >= 'A' && *buf <= 'F')
			val += *buf - 'A' + 10;
		else if (*buf >= 'a' && *buf <= 'f')
			val += *buf - 'a' + 10;
		else
			return (unsigned)-1;
		buf++;
		tally++;
		if (count && tally == count)
			return val;
		if (!count && *buf == '\0')
			return val;
	}
}

static bool
process_srecord(const char *buf, uint8_t *count, uint16_t *address, uint8_t *data, uint8_t *checksum)
{
	unsigned hexnum, pos = 2;

	if ((hexnum = hex(buf + pos, 2)) > 0xFF)
		return false;
	uint8_t index = hexnum;
	*count = index;
	uint16_t sum = index;
	index -= 3;
	pos += 2;
	if ((hexnum = hex(buf + pos, 4)) > 0xFFFF)
		return false;
	const uint16_t addr = hexnum;
	*address = addr;
	sum += (addr >> 8) & 0xFF;
	sum += (addr & 0xFF);
	pos += 4;
	for (unsigned i = 0; i < index; i++) {
		if ((hexnum = hex(buf + pos, 2)) > 0xFF)
			return false;
		const uint8_t val = hexnum;
		data[i] = val;
		sum += val;
		pos += 2;
	}
	sum = 0x00FF - (uint8_t)sum;
	if ((hexnum = hex(buf + pos, 2)) > 0xFF)
		return false;
	*checksum = hexnum;
	return *checksum == sum;
}

void GenericMemory::load_srec(const char *filename, Word base)
{
	FILE		*fp;
	bool		done = false;
	char		buffer[1024];

	fp = fopen(filename, "r");
	if (!fp) {
		perror("filename");
		exit(EXIT_FAILURE);
	}

	while (!done && fgets(buffer, 1023, fp)) {
		uint8_t		data[256], count, checksum;
		uint16_t	address;

		if (process_srecord(buffer, &count, &address, data, &checksum)) {
			if (buffer[1] == '1') {
				for (uint16_t i = 0u; i < (uint16_t)count; i++) {
					if ((address >= base) && (address < ((DWord)base + size))) {
						memory[address - base] = data[i];
						++address;
					}
				}
			} else if (buffer[1] == '9')
				done = true;
		}
	}
	fclose(fp);
}

//
// ELF32 loader. Mirrors the inline parser in MAME's llvm6309 driver:
// validate e_ident, walk PT_LOAD program headers, memcpy each loadable
// segment into memory[]. Both endiannesses are supported because the
// 6809 toolchain emits MSB (matching the CPU's natural byte order)
// while many host-side tools default to LSB.
//
// Out-of-range PT_LOADs are silently dropped, like load_intelhex /
// load_srec — so a single ELF can populate two physically separate
// devices by being passed to each in turn (e.g. a loader RAM + a
// FRAM device, or a watchpoint shadow), with each device picking up
// only the segments that fall in its window.
//

static inline uint16_t elf_rd16_le(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t elf_rd32_le(const uint8_t *p) {
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}
static inline uint16_t elf_rd16_be(const uint8_t *p) {
	return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
static inline uint32_t elf_rd32_be(const uint8_t *p) {
	return ((uint32_t)p[0] << 24)
	     | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] << 8)
	     |  (uint32_t)p[3];
}

void GenericMemory::load_elf(const char *filename, Word base, Word start_vector_addr)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		perror("filename");
		exit(EXIT_FAILURE);
	}

	// Read whole file. ELFs may be many MB once `.debug_*` sections
	// are present, but that's still fine for a host-side load.
	fseek(fp, 0, SEEK_END);
	long flen = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (flen < 52) {
		fprintf(stderr, "load_elf: %s: header truncated (%ld bytes)\n", filename, flen);
		fclose(fp);
		exit(EXIT_FAILURE);
	}
	std::vector<uint8_t> buf((size_t)flen);
	if (fread(buf.data(), 1, (size_t)flen, fp) != (size_t)flen) {
		perror("load_elf: short read");
		fclose(fp);
		exit(EXIT_FAILURE);
	}
	fclose(fp);

	const uint8_t *elf = buf.data();
	if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
		fprintf(stderr, "load_elf: %s: not an ELF file\n", filename);
		exit(EXIT_FAILURE);
	}
	if (elf[4] != 1) {
		fprintf(stderr, "load_elf: %s: not ELFCLASS32\n", filename);
		exit(EXIT_FAILURE);
	}
	bool be;
	if (elf[5] == 2)      be = true;
	else if (elf[5] == 1) be = false;
	else {
		fprintf(stderr, "load_elf: %s: invalid EI_DATA (%u)\n", filename, elf[5]);
		exit(EXIT_FAILURE);
	}

	auto rd16 = [be](const uint8_t *p) {
		return be ? elf_rd16_be(p) : elf_rd16_le(p);
	};
	auto rd32 = [be](const uint8_t *p) {
		return be ? elf_rd32_be(p) : elf_rd32_le(p);
	};

	uint32_t e_phoff     = rd32(elf + 28);
	uint32_t e_shoff     = rd32(elf + 32);
	uint16_t e_phentsize = rd16(elf + 42);
	uint16_t e_phnum     = rd16(elf + 44);
	uint16_t e_shentsize = rd16(elf + 46);
	uint16_t e_shnum     = rd16(elf + 48);

	if (e_phentsize < 32) {
		fprintf(stderr, "load_elf: %s: program header size %u too small\n",
			filename, e_phentsize);
		exit(EXIT_FAILURE);
	}

	// Walk PT_LOAD program headers, dropping segments outside our window.
	for (uint16_t i = 0; i < e_phnum; i++) {
		uint32_t phoff = e_phoff + (uint32_t)i * e_phentsize;
		if (phoff + 32 > (uint32_t)flen) {
			fprintf(stderr, "load_elf: %s: truncated program header %u\n",
				filename, (unsigned)i);
			exit(EXIT_FAILURE);
		}
		const uint8_t *ph = elf + phoff;

		uint32_t p_type   = rd32(ph + 0);
		uint32_t p_offset = rd32(ph + 4);
		uint32_t p_vaddr  = rd32(ph + 8);
		uint32_t p_filesz = rd32(ph + 16);

		if (p_type != 1 /* PT_LOAD */ || p_filesz == 0)
			continue;
		if (p_offset > (uint32_t)flen || p_offset + p_filesz > (uint32_t)flen) {
			fprintf(stderr, "load_elf: %s: PT_LOAD %u past EOF\n",
				filename, (unsigned)i);
			exit(EXIT_FAILURE);
		}

		// Trim the segment to the intersection of [p_vaddr, p_vaddr+p_filesz)
		// with [base, base+size). The whole segment may lie outside —
		// just like an Intel HEX record can.
		uint32_t seg_lo = p_vaddr;
		uint32_t seg_hi = p_vaddr + p_filesz;
		uint32_t win_lo = (uint32_t)base;
		uint32_t win_hi = (uint32_t)base + (uint32_t)size;
		if (seg_hi <= win_lo || seg_lo >= win_hi)
			continue;
		uint32_t lo = (seg_lo < win_lo) ? win_lo : seg_lo;
		uint32_t hi = (seg_hi > win_hi) ? win_hi : seg_hi;
		std::memcpy(&memory[lo - win_lo],
			    elf + p_offset + (lo - seg_lo),
			    hi - lo);
	}

	// Reset-vector fallback. If the vector address is in our range and
	// no PT_LOAD covered it, look for `_start` in the symbol table and
	// write its address there as a big-endian word.
	if (start_vector_addr == 0)
		return;
	uint32_t vec_lo = (uint32_t)start_vector_addr;
	uint32_t win_lo = (uint32_t)base;
	uint32_t win_hi = (uint32_t)base + (uint32_t)size;
	if (vec_lo + 1 < win_lo || vec_lo + 1 >= win_hi)
		return;
	if (memory[vec_lo - win_lo] != 0 || memory[vec_lo + 1 - win_lo] != 0)
		return;	// PT_LOAD already populated the vector

	if (e_shoff == 0 || e_shnum == 0 || e_shentsize < 40)
		return;

	uint32_t symtab_off = 0, symtab_size = 0, symtab_entsize = 0;
	uint32_t symtab_link = 0;
	for (uint16_t i = 0; i < e_shnum; i++) {
		uint32_t shoff = e_shoff + (uint32_t)i * e_shentsize;
		if (shoff + 40 > (uint32_t)flen) break;
		const uint8_t *sh = elf + shoff;
		uint32_t sh_type = rd32(sh + 4);
		if (sh_type == 2 /* SHT_SYMTAB */) {
			symtab_off     = rd32(sh + 16);
			symtab_size    = rd32(sh + 20);
			symtab_link    = rd32(sh + 24);
			symtab_entsize = rd32(sh + 36);
			break;
		}
	}
	if (symtab_off == 0 || symtab_entsize < 16
	    || symtab_off + symtab_size > (uint32_t)flen
	    || symtab_link == 0 || symtab_link >= e_shnum)
		return;

	uint32_t strtab_shoff = e_shoff + symtab_link * e_shentsize;
	if (strtab_shoff + 40 > (uint32_t)flen)
		return;
	uint32_t strtab_off  = rd32(elf + strtab_shoff + 16);
	uint32_t strtab_size = rd32(elf + strtab_shoff + 20);
	if (strtab_off == 0 || strtab_off + strtab_size > (uint32_t)flen)
		return;
	const char *strtab = (const char *)(elf + strtab_off);

	for (uint32_t off = 0; off + 16 <= symtab_size; off += symtab_entsize) {
		const uint8_t *sym = elf + symtab_off + off;
		uint32_t st_name  = rd32(sym + 0);
		uint32_t st_value = rd32(sym + 4);
		if (st_name == 0 || st_name >= strtab_size)
			continue;
		if (std::strcmp(strtab + st_name, "_start") != 0)
			continue;
		if (st_value >= 0x10000)
			break;
		memory[vec_lo     - win_lo] = (Byte)((st_value >> 8) & 0xFF);
		memory[vec_lo + 1 - win_lo] = (Byte)(st_value & 0xFF);
		break;
	}
}
