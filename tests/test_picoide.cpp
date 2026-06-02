//
//	test_picoide.cpp
//	Unit tests for PicoIDE focused on multi-sector READ/WRITE,
//	which the pre-fix implementation truncated to a single sector
//	regardless of the sector_count register.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "picoide.h"

static int failures = 0;

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
				__FILE__, __LINE__, #cond);		\
			++failures;					\
		}							\
	} while (0)

static const Byte SR_DRDY = 0x40;
static const Byte SR_DRQ  = 0x08;
static const Byte SR_ERR  = 0x01;

static const Byte CMD_READ     = 0x20;
static const Byte CMD_WRITE    = 0x30;
static const Byte CMD_IDENTIFY = 0xEC;

// Register offsets within $FF00-$FF09.
static const Word REG_DATA_HI = 0;
static const Word REG_DATA_LO = 1;
static const Word REG_SECCNT  = 3;
static const Word REG_LBA_LO  = 4;
static const Word REG_LBA_MID = 5;
static const Word REG_LBA_HI  = 6;
static const Word REG_DRVHEAD = 7;
static const Word REG_STATUS  = 8;
static const Word REG_CMD     = 8;

// Build a 1MB test image populated with a per-sector pattern: sector N
// is filled with the byte (N ^ (N >> 8)) so each sector has a distinct
// signature distinguishable from its neighbours.
static char* make_test_image(unsigned num_sectors)
{
	static char path[64];
	snprintf(path, sizeof(path), "/tmp/picoide_test_XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); exit(EXIT_FAILURE); }

	uint8_t sector[512];
	for (unsigned s = 0; s < num_sectors; s++) {
		Byte pat = (Byte)((s ^ (s >> 8)) & 0xFF);
		memset(sector, pat, sizeof(sector));
		// Stamp the LBA into the first 4 bytes for a stronger signature.
		sector[0] = (Byte)(s & 0xFF);
		sector[1] = (Byte)((s >> 8) & 0xFF);
		sector[2] = (Byte)((s >> 16) & 0xFF);
		sector[3] = (Byte)((s >> 24) & 0xFF);
		if (write(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
			perror("write"); exit(EXIT_FAILURE);
		}
	}
	close(fd);
	return path;
}

static void set_lba(PicoIDE& ide, uint32_t lba)
{
	ide.write(REG_LBA_LO,  (Byte)(lba & 0xFF));
	ide.write(REG_LBA_MID, (Byte)((lba >> 8) & 0xFF));
	ide.write(REG_LBA_HI,  (Byte)((lba >> 16) & 0xFF));
	ide.write(REG_DRVHEAD, (Byte)(0xE0 | ((lba >> 24) & 0x0F)));	// LBA mode
}

// Drain a full 512-byte sector from the data register (hi/lo pairing).
static void read_sector_bytes(PicoIDE& ide, uint8_t out[512])
{
	for (unsigned i = 0; i < 512; i += 2) {
		out[i]     = ide.read(REG_DATA_HI);
		out[i + 1] = ide.read(REG_DATA_LO);
	}
}

static void write_sector_bytes(PicoIDE& ide, const uint8_t in[512])
{
	for (unsigned i = 0; i < 512; i += 2) {
		ide.write(REG_DATA_HI, in[i]);
		ide.write(REG_DATA_LO, in[i + 1]);
	}
}

static void test_single_sector_read()
{
	char* path = make_test_image(4);
	PicoIDE ide(path);

	set_lba(ide, 2);
	ide.write(REG_SECCNT, 1);
	ide.write(REG_CMD, CMD_READ);

	CHECK((ide.read(REG_STATUS) & SR_DRQ) != 0);
	CHECK((ide.read(REG_STATUS) & SR_ERR) == 0);

	uint8_t sec[512];
	read_sector_bytes(ide, sec);

	const Byte pat = (Byte)2;	// sector 2: (s ^ (s>>8)) = 2
	CHECK(sec[0] == 2);		// LBA signature
	CHECK(sec[4] == pat);		// fill pattern (past the 4-byte LBA stamp)
	CHECK(sec[511] == pat);

	// After draining the only sector, DRQ must drop.
	CHECK((ide.read(REG_STATUS) & SR_DRQ) == 0);
	CHECK((ide.read(REG_STATUS) & SR_DRDY) != 0);
	CHECK(ide.read(REG_SECCNT) == 0);

	unlink(path);
}

static void test_multi_sector_read_streams_each_sector_in_one_command()
{
	const unsigned start = 5;
	const unsigned count = 4;
	char* path = make_test_image(start + count + 2);
	PicoIDE ide(path);

	set_lba(ide, start);
	ide.write(REG_SECCNT, count);
	ide.write(REG_CMD, CMD_READ);

	for (unsigned s = 0; s < count; s++) {
		// DRQ must be asserted before each sector.
		CHECK((ide.read(REG_STATUS) & SR_DRQ) != 0);
		CHECK(ide.read(REG_SECCNT) == (Byte)(count - s));

		uint8_t sec[512];
		read_sector_bytes(ide, sec);

		unsigned lba = start + s;
		Byte pat = (Byte)((lba ^ (lba >> 8)) & 0xFF);
		CHECK(sec[0] == (Byte)(lba & 0xFF));	// LBA signature
		CHECK(sec[4] == pat);
		CHECK(sec[511] == pat);
	}

	// After the last sector drains, DRQ drops and seccnt reads 0.
	CHECK((ide.read(REG_STATUS) & SR_DRQ) == 0);
	CHECK((ide.read(REG_STATUS) & SR_DRDY) != 0);
	CHECK(ide.read(REG_SECCNT) == 0);

	unlink(path);
}

static void test_multi_sector_write_streams_each_sector_in_one_command()
{
	const unsigned start = 3;
	const unsigned count = 3;
	char* path = make_test_image(start + count + 2);
	PicoIDE ide(path);

	set_lba(ide, start);
	ide.write(REG_SECCNT, count);
	ide.write(REG_CMD, CMD_WRITE);

	for (unsigned s = 0; s < count; s++) {
		// DRQ asserted, drive ready to receive each sector.
		CHECK((ide.read(REG_STATUS) & SR_DRQ) != 0);
		CHECK(ide.read(REG_SECCNT) == (Byte)(count - s));

		uint8_t sec[512];
		// Write a per-sector marker: sector S filled with $A0+s, with
		// a sentinel word at the start so we can tell sectors apart.
		memset(sec, (Byte)(0xA0 + s), sizeof(sec));
		sec[0] = 0xDE;
		sec[1] = (Byte)s;
		write_sector_bytes(ide, sec);
	}

	// After last sector flushes, DRQ drops, seccnt 0, status DRDY.
	CHECK((ide.read(REG_STATUS) & SR_DRQ) == 0);
	CHECK((ide.read(REG_STATUS) & SR_DRDY) != 0);
	CHECK(ide.read(REG_SECCNT) == 0);

	// Verify each sector on disk by reading back through PicoIDE.
	for (unsigned s = 0; s < count; s++) {
		set_lba(ide, start + s);
		ide.write(REG_SECCNT, 1);
		ide.write(REG_CMD, CMD_READ);

		uint8_t sec[512];
		read_sector_bytes(ide, sec);
		CHECK(sec[0] == 0xDE);
		CHECK(sec[1] == (Byte)s);
		CHECK(sec[2] == (Byte)(0xA0 + s));
		CHECK(sec[511] == (Byte)(0xA0 + s));
	}

	unlink(path);
}

static void test_seccnt_zero_means_256()
{
	// Build an image of exactly 256 sectors; multi-read with seccnt=0
	// must transfer all 256. We only sanity-check that DRQ persists
	// through the expected count and that the LBA signatures advance,
	// rather than fully draining (130KB of reads in a unit test is fine,
	// but checking every sector individually is overkill — three points
	// is enough to catch a regression).
	char* path = make_test_image(256);
	PicoIDE ide(path);

	set_lba(ide, 0);
	ide.write(REG_SECCNT, 0);
	ide.write(REG_CMD, CMD_READ);

	for (unsigned s = 0; s < 256; s++) {
		CHECK((ide.read(REG_STATUS) & SR_DRQ) != 0);
		uint8_t sec[512];
		read_sector_bytes(ide, sec);
		if (s == 0 || s == 1 || s == 255) {
			CHECK(sec[0] == (Byte)(s & 0xFF));
			CHECK(sec[1] == (Byte)((s >> 8) & 0xFF));
		}
	}
	CHECK((ide.read(REG_STATUS) & SR_DRQ) == 0);
	CHECK(ide.read(REG_SECCNT) == 0);

	unlink(path);
}

static void test_identify_drops_drq_after_one_sector()
{
	char* path = make_test_image(1);
	PicoIDE ide(path);

	ide.write(REG_CMD, CMD_IDENTIFY);
	CHECK((ide.read(REG_STATUS) & SR_DRQ) != 0);

	uint8_t sec[512];
	read_sector_bytes(ide, sec);
	// Model string at words 27-46 → byte offset 54-93; expect 'P'/'i'.
	CHECK(sec[55] == 'P');
	CHECK(sec[54] == 'i');

	CHECK((ide.read(REG_STATUS) & SR_DRQ) == 0);
	CHECK((ide.read(REG_STATUS) & SR_DRDY) != 0);

	unlink(path);
}

static void test_no_disk_aborts_with_error()
{
	PicoIDE ide(nullptr);
	ide.write(REG_SECCNT, 1);
	ide.write(REG_CMD, CMD_READ);
	CHECK((ide.read(REG_STATUS) & SR_ERR) != 0);
	CHECK((ide.read(REG_STATUS) & SR_DRQ) == 0);

	ide.write(REG_CMD, CMD_WRITE);
	CHECK((ide.read(REG_STATUS) & SR_ERR) != 0);
	CHECK((ide.read(REG_STATUS) & SR_DRQ) == 0);
}

int main()
{
	test_single_sector_read();
	test_multi_sector_read_streams_each_sector_in_one_command();
	test_multi_sector_write_streams_each_sector_in_one_command();
	test_seccnt_zero_means_256();
	test_identify_drops_drq_after_one_sector();
	test_no_disk_aborts_with_error();

	if (failures) {
		fprintf(stderr, "FAILED: %d assertion(s)\n", failures);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "OK\n");
	return EXIT_SUCCESS;
}
