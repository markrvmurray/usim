//
//	picoide.cpp
//	Pico-Thing PATA IDE Controller
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstring>
#include "picoide.h"

PicoIDE::PicoIDE(const char* image_path)
	: disk(nullptr), buf_ptr(0), writing(false),
	  error_reg(0), features(0), sector_count(1),
	  lba_low(0), lba_mid(0), lba_high(0),
	  drive_head(0xA0), status(SR_DRDY), device_control(0)
{
	memset(buffer, 0, sizeof(buffer));
	if (image_path) {
		disk = fopen(image_path, "r+b");
		if (!disk) {
			// Try creating it
			disk = fopen(image_path, "w+b");
		}
		if (!disk) {
			fprintf(stderr, "picoide: cannot open disk image: %s\n", image_path);
		}
	}
}

PicoIDE::~PicoIDE()
{
	if (disk)
		fclose(disk);
}

uint32_t PicoIDE::get_lba()
{
	return ((uint32_t)(drive_head & 0x0F) << 24) |
	       ((uint32_t)lba_high << 16) |
	       ((uint32_t)lba_mid << 8) |
	       (uint32_t)lba_low;
}

void PicoIDE::do_read_sectors()
{
	if (!disk) {
		status = SR_DRDY | SR_ERR;
		error_reg = 0x04;	// abort
		return;
	}

	uint32_t lba = get_lba();
	long offset = (long)lba * 512;

	fseek(disk, offset, SEEK_SET);
	size_t n = fread(buffer, 1, 512, disk);
	if (n < 512)
		memset(buffer + n, 0, 512 - n);	// zero-pad short reads

	buf_ptr = 0;
	writing = false;
	status = SR_DRDY | SR_DRQ;
}

void PicoIDE::do_write_sectors()
{
	memset(buffer, 0, sizeof(buffer));
	buf_ptr = 0;
	writing = true;
	status = SR_DRDY | SR_DRQ;
}

void PicoIDE::complete_write()
{
	if (!disk) {
		status = SR_DRDY | SR_ERR;
		error_reg = 0x04;
		return;
	}

	uint32_t lba = get_lba();
	long offset = (long)lba * 512;

	fseek(disk, offset, SEEK_SET);
	(void)fwrite(buffer, 1, 512, disk);
	fflush(disk);

	writing = false;
	status = SR_DRDY;
}

void PicoIDE::do_identify()
{
	memset(buffer, 0, sizeof(buffer));

	// Word 0: General configuration
	buffer[0] = 0x00; buffer[1] = 0x40;	// fixed disk

	// Words 1-3: Cylinders, heads, sectors (legacy CHS)
	buffer[2] = 0x04; buffer[3] = 0x00;	// 1024 cylinders
	buffer[6] = 0x00; buffer[7] = 0x10;	// 16 heads
	buffer[8] = 0x00; buffer[9] = 0x3F;	// 63 sectors/track

	// Words 27-46: Model string (40 chars, swapped pairs)
	const char* model = "PicoThing Virtual HD                    ";
	for (int i = 0; i < 40; i += 2) {
		buffer[54 + i] = model[i + 1];
		buffer[54 + i + 1] = model[i];
	}

	// Word 49: Capabilities (LBA supported)
	buffer[98] = 0x02; buffer[99] = 0x00;	// LBA supported

	// Words 60-61: Total sectors (LBA28) - 1GB = 2097152 sectors
	buffer[120] = 0x20; buffer[121] = 0x00;
	buffer[122] = 0x00; buffer[123] = 0x00;

	buf_ptr = 0;
	writing = false;
	status = SR_DRDY | SR_DRQ;
}

Byte PicoIDE::read(Word offset)
{
	switch (offset) {
	case 0:		// Data register high byte
		if (status & SR_DRQ) {
			return buffer[buf_ptr];
		}
		return 0xFF;

	case 1:		// Data register low byte
		if (status & SR_DRQ) {
			Byte val = buffer[buf_ptr + 1];
			buf_ptr += 2;
			if (buf_ptr >= 512) {
				status &= ~SR_DRQ;
				status |= SR_DRDY;
			}
			return val;
		}
		return 0xFF;

	case 2:		return error_reg;
	case 3:		return sector_count;
	case 4:		return lba_low;
	case 5:		return lba_mid;
	case 6:		return lba_high;
	case 7:		return drive_head;
	case 8:		return status;
	case 9:		return status;		// alt status (no side effects)
	default:	return 0xFF;
	}
}

void PicoIDE::write(Word offset, Byte val)
{
	switch (offset) {
	case 0:		// Data register high byte
		if ((status & SR_DRQ) && writing) {
			buffer[buf_ptr] = val;
		}
		break;

	case 1:		// Data register low byte
		if ((status & SR_DRQ) && writing) {
			buffer[buf_ptr + 1] = val;
			buf_ptr += 2;
			if (buf_ptr >= 512) {
				complete_write();
			}
		}
		break;

	case 2:		features = val; break;
	case 3:		sector_count = val; break;
	case 4:		lba_low = val; break;
	case 5:		lba_mid = val; break;
	case 6:		lba_high = val; break;
	case 7:		drive_head = val; break;

	case 8:		// Command register
		error_reg = 0;
		switch (val) {
		case CMD_READ:
			do_read_sectors();
			break;
		case CMD_WRITE:
			do_write_sectors();
			break;
		case CMD_IDENTIFY:
			do_identify();
			break;
		default:
			status = SR_DRDY | SR_ERR;
			error_reg = 0x04;	// command aborted
			break;
		}
		break;

	case 9:		// Device control
		device_control = val;
		break;
	}
}
