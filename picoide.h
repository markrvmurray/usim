//
//	picoide.h
//	Pico-Thing PATA IDE Controller
//
//	10 bytes at $FF00-$FF09. File-backed disk image(s).
//
//	Register map (Pico-Thing layout, shifted +1 from standard ATA):
//	  Offset 0-1: Data register (16-bit, big-endian on 6809 bus)
//	  Offset 2:   Error (read) / Features (write)
//	  Offset 3:   Sector Count
//	  Offset 4:   LBA Low  (Sector Number)
//	  Offset 5:   LBA Mid  (Cylinder Low)
//	  Offset 6:   LBA High (Cylinder High)
//	  Offset 7:   Drive/Head  (bit 4 = DEV: 0=master, 1=slave)
//	  Offset 8:   Status (read) / Command (write)
//	  Offset 9:   Alt Status (read) / Device Control (write)
//
//	Two drives share the bus, the standard ATA way: every taskfile
//	write (features, seccnt, LBA bytes, drive/head shadow, device
//	control) goes to BOTH drives' shadow registers, but only the
//	selected drive responds to commands and presents data, status,
//	and error. The selector is bit 4 of any value written to the
//	Drive/Head register. A drive whose slot is unpopulated returns
//	status $00 — that's how a host probe learns the slot is empty.
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

	// Per-drive state. Taskfile registers are duplicated rather than
	// shared so that the (rarely-used but legal) sequence "set LBA on
	// A, switch DEV to B, command B" doesn't accidentally smuggle A's
	// LBA across — both shadows are updated by every taskfile write,
	// so the registers track in lock-step under normal use anyway.
	struct Drive {
		FILE*		disk = nullptr;
		bool		present = false;
		uint8_t		buffer[512] = {};
		uint16_t	buf_ptr = 0;
		bool		writing = false;
		uint16_t	sectors_remaining = 0;
		uint32_t	current_lba = 0;
		Byte		error_reg = 0;
		Byte		features = 0;
		Byte		sector_count = 1;
		Byte		lba_low = 0;
		Byte		lba_mid = 0;
		Byte		lba_high = 0;
		Byte		drive_head = 0xA0;
		Byte		status = SR_DRDY;
		Byte		device_control = 0;
	};

	Drive		drives[2];
	int		selected = 0;	// 0 = master, 1 = slave

	Drive&		sel()			{ return drives[selected]; }
	uint32_t	get_lba(const Drive& d) const;
	void		do_read_sectors();
	void		do_write_sectors();
	void		do_identify();
	void		complete_write();
	void		load_sector_at_current_lba();

public:
			PicoIDE(const char* master_image, const char* slave_image = nullptr);
	virtual		~PicoIDE();

	Byte		read(Word offset) override;
	void		write(Word offset, Byte val) override;
};
