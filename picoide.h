//
//	picoide.h
//	Pico-Thing PATA IDE Controller
//
//	10 bytes at $FF00-$FF09. File-backed disk image.
//
//	Register map (Pico-Thing layout, shifted +1 from standard ATA):
//	  Offset 0-1: Data register (16-bit, big-endian on 6809 bus)
//	  Offset 2:   Error (read) / Features (write)
//	  Offset 3:   Sector Count
//	  Offset 4:   LBA Low  (Sector Number)
//	  Offset 5:   LBA Mid  (Cylinder Low)
//	  Offset 6:   LBA High (Cylinder High)
//	  Offset 7:   Drive/Head
//	  Offset 8:   Status (read) / Command (write)
//	  Offset 9:   Alt Status (read) / Device Control (write)
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <cstdio>
#include "device.h"

class PicoIDE : public MappedDevice {

	// Status register bits
	static const Byte	SR_BSY  = 0x80;
	static const Byte	SR_DRDY = 0x40;
	static const Byte	SR_DRQ  = 0x08;
	static const Byte	SR_ERR  = 0x01;

	// ATA commands
	static const Byte	CMD_READ    = 0x20;
	static const Byte	CMD_WRITE   = 0x30;
	static const Byte	CMD_IDENTIFY = 0xEC;

	FILE*		disk;
	uint8_t		buffer[512];
	uint16_t	buf_ptr;	// byte index into buffer
	bool		writing;	// true during write data transfer

	// Multi-sector transfer state. A single READ/WRITE command can
	// move up to 256 sectors (sector_count register, with 0 meaning
	// 256 per ATA convention). `sectors_remaining` counts sectors not
	// yet transferred to/from the host *including* the one in the
	// buffer; it drops to zero when DRQ goes low. `current_lba` is
	// the LBA of the sector currently in the buffer (or about to be
	// fetched/written), bumped per sector across the command.
	uint16_t	sectors_remaining;
	uint32_t	current_lba;

	Byte		error_reg;
	Byte		features;
	Byte		sector_count;
	Byte		lba_low;
	Byte		lba_mid;
	Byte		lba_high;
	Byte		drive_head;
	Byte		status;
	Byte		device_control;

	uint32_t	get_lba();
	void		do_read_sectors();
	void		do_write_sectors();
	void		do_identify();
	void		complete_write();
	void		load_sector_at_current_lba();	// read 512 bytes into buffer

public:
			PicoIDE(const char* image_path);
	virtual		~PicoIDE();

	Byte		read(Word offset) override;
	void		write(Word offset, Byte val) override;
};
