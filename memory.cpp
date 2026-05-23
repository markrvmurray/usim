//
//	memory.c
//	(C) R.P.Bellis 2021 - 2025
//
//      vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdint>
#include <cstdlib>
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
