//
//	picoide.cpp
//	Pico-Thing PATA IDE Controller
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstring>
#include "picoide.h"

PicoIDE::PicoIDE(const char* master_image, const char* slave_image)
{
	// Master slot is always present so the controller behaves
	// identically to the pre-two-drive code when no slave is wired:
	// status reads $40 (DRDY), commands abort if there's no backing
	// image. Open-or-create the image, mirroring the original.
	drives[0].present = true;
	if (master_image) {
		drives[0].disk = fopen(master_image, "r+b");
		if (!drives[0].disk)
			drives[0].disk = fopen(master_image, "w+b");
		if (!drives[0].disk)
			fprintf(stderr, "picoide: cannot open master image: %s\n",
				master_image);
	}

	// Slave slot is only present if a path was given AND the file
	// opens successfully. No auto-create on the slave: a stray flag
	// typo shouldn't silently spawn a 0-byte image. If the host
	// never wires a slave, DEV=1 selects an absent slot, status
	// reads $00, and the host probe learns the slot is empty — the
	// standard ATA way of detecting a single-drive bus.
	if (slave_image) {
		drives[1].disk = fopen(slave_image, "r+b");
		if (!drives[1].disk) {
			fprintf(stderr, "picoide: cannot open slave image: %s\n",
				slave_image);
		} else {
			drives[1].present = true;
		}
	}
}

PicoIDE::~PicoIDE()
{
	for (auto& d : drives) {
		if (d.disk) fclose(d.disk);
	}
}

uint32_t PicoIDE::get_lba(const Drive& d) const
{
	return ((uint32_t)(d.drive_head & 0x0F) << 24) |
	       ((uint32_t)d.lba_high << 16) |
	       ((uint32_t)d.lba_mid << 8) |
	       (uint32_t)d.lba_low;
}

void PicoIDE::load_sector_at_current_lba()
{
	Drive& d = sel();
	long offset = (long)d.current_lba * 512;
	fseek(d.disk, offset, SEEK_SET);
	size_t n = fread(d.buffer, 1, 512, d.disk);
	if (n < 512)
		memset(d.buffer + n, 0, 512 - n);	// zero-pad short reads
	d.buf_ptr = 0;
	d.status = SR_DRDY | SR_DRQ;
}

void PicoIDE::do_read_sectors()
{
	Drive& d = sel();
	if (!d.disk) {
		d.status = SR_DRDY | SR_ERR;
		d.error_reg = 0x04;	// abort
		d.sectors_remaining = 0;
		d.sector_count = 0;
		return;
	}

	d.current_lba = get_lba(d);
	// ATA convention: sector_count == 0 means 256 sectors.
	d.sectors_remaining = (d.sector_count == 0) ? 256 : d.sector_count;
	d.writing = false;
	load_sector_at_current_lba();
}

void PicoIDE::do_write_sectors()
{
	Drive& d = sel();
	if (!d.disk) {
		d.status = SR_DRDY | SR_ERR;
		d.error_reg = 0x04;
		d.sectors_remaining = 0;
		d.sector_count = 0;
		return;
	}

	d.current_lba = get_lba(d);
	d.sectors_remaining = (d.sector_count == 0) ? 256 : d.sector_count;
	memset(d.buffer, 0, sizeof(d.buffer));
	d.buf_ptr = 0;
	d.writing = true;
	d.status = SR_DRDY | SR_DRQ;
}

void PicoIDE::complete_write()
{
	Drive& d = sel();
	if (!d.disk) {
		d.status = SR_DRDY | SR_ERR;
		d.error_reg = 0x04;
		d.writing = false;
		d.sectors_remaining = 0;
		d.sector_count = 0;
		return;
	}

	long offset = (long)d.current_lba * 512;
	fseek(d.disk, offset, SEEK_SET);
	(void)fwrite(d.buffer, 1, 512, d.disk);
	fflush(d.disk);

	++d.current_lba;
	if (d.sectors_remaining > 0) --d.sectors_remaining;
	d.sector_count = (Byte)d.sectors_remaining;

	if (d.sectors_remaining > 0) {
		memset(d.buffer, 0, sizeof(d.buffer));
		d.buf_ptr = 0;
		// status stays SR_DRDY | SR_DRQ — host streams next sector
	} else {
		d.writing = false;
		d.status = SR_DRDY;
	}
}

void PicoIDE::do_identify()
{
	Drive& d = sel();
	memset(d.buffer, 0, sizeof(d.buffer));

	// Word 0: General configuration
	d.buffer[0] = 0x00; d.buffer[1] = 0x40;	// fixed disk

	// Words 1-3: Cylinders, heads, sectors (legacy CHS)
	d.buffer[2] = 0x04; d.buffer[3] = 0x00;	// 1024 cylinders
	d.buffer[6] = 0x00; d.buffer[7] = 0x10;	// 16 heads
	d.buffer[8] = 0x00; d.buffer[9] = 0x3F;	// 63 sectors/track

	// Words 27-46: Model string (40 chars, swapped pairs)
	const char* model = "PicoThing Virtual HD                    ";
	for (int i = 0; i < 40; i += 2) {
		d.buffer[54 + i] = model[i + 1];
		d.buffer[54 + i + 1] = model[i];
	}

	// Word 49: Capabilities (LBA supported)
	d.buffer[98] = 0x02; d.buffer[99] = 0x00;

	// Words 60-61: Total sectors (LBA28) - 1GB = 2097152 sectors
	d.buffer[120] = 0x20; d.buffer[121] = 0x00;
	d.buffer[122] = 0x00; d.buffer[123] = 0x00;

	d.buf_ptr = 0;
	d.writing = false;
	// IDENTIFY is a single-sector pseudo-transfer.
	d.sectors_remaining = 1;
	d.status = SR_DRDY | SR_DRQ;
}

Byte PicoIDE::read(Word offset)
{
	Drive& d = sel();

	// Absent slot reads $00 for status/altstatus — the conventional
	// "no device" signature a host probe checks for. Taskfile bytes
	// still echo the shared shadow so a host that reads them back
	// can verify its writes regardless of which slot is selected.
	if (!d.present && (offset == 8 || offset == 9))
		return 0x00;

	switch (offset) {
	case 0:		// Data register high byte
		if (d.status & SR_DRQ)
			return d.buffer[d.buf_ptr];
		return 0xFF;

	case 1:		// Data register low byte — advances buffer pointer
		if (d.status & SR_DRQ) {
			Byte val = d.buffer[d.buf_ptr + 1];
			d.buf_ptr += 2;
			if (d.buf_ptr >= 512) {
				// Current sector fully transferred to host.
				if (d.sectors_remaining > 0)
					--d.sectors_remaining;
				d.sector_count = (Byte)d.sectors_remaining;
				if (d.sectors_remaining > 0 && !d.writing && d.disk) {
					++d.current_lba;
					load_sector_at_current_lba();
				} else {
					d.status &= ~SR_DRQ;
					d.status |= SR_DRDY;
				}
			}
			return val;
		}
		return 0xFF;

	case 2:		return d.error_reg;
	case 3:		return d.sector_count;
	case 4:		return d.lba_low;
	case 5:		return d.lba_mid;
	case 6:		return d.lba_high;
	case 7:		return d.drive_head;
	case 8:		return d.status;
	case 9:		return d.status;	// alt status (no side effects)
	default:	return 0xFF;
	}
}

void PicoIDE::write(Word offset, Byte val)
{
	switch (offset) {
	case 0:		// Data register high byte — selected drive only
		{
			Drive& d = sel();
			if ((d.status & SR_DRQ) && d.writing)
				d.buffer[d.buf_ptr] = val;
		}
		break;

	case 1:		// Data register low byte — selected drive only
		{
			Drive& d = sel();
			if ((d.status & SR_DRQ) && d.writing) {
				d.buffer[d.buf_ptr + 1] = val;
				d.buf_ptr += 2;
				if (d.buf_ptr >= 512)
					complete_write();
			}
		}
		break;

	// Taskfile writes go to BOTH drives' shadow copies — the standard
	// ATA wiring. The deselected drive needs to track these so that a
	// later DEV swap exposes the latched value.
	case 2:		for (auto& d : drives) d.features     = val; break;
	case 3:		for (auto& d : drives) d.sector_count = val; break;
	case 4:		for (auto& d : drives) d.lba_low      = val; break;
	case 5:		for (auto& d : drives) d.lba_mid      = val; break;
	case 6:		for (auto& d : drives) d.lba_high     = val; break;

	case 7:		// Drive/Head — DEV select lives in bit 4
		for (auto& d : drives) d.drive_head = val;
		selected = (val >> 4) & 1;
		break;

	case 8:		// Command register — issued to selected drive only
		{
			Drive& d = sel();
			d.error_reg = 0;
			switch (val) {
			case CMD_READ:		do_read_sectors();  break;
			case CMD_WRITE:		do_write_sectors(); break;
			case CMD_IDENTIFY:	do_identify();      break;
			default:
				d.status = SR_DRDY | SR_ERR;
				d.error_reg = 0x04;	// command aborted
				break;
			}
		}
		break;

	case 9:		// Device control — broadcast (it really is shared)
		for (auto& d : drives) d.device_control = val;
		break;
	}
}
