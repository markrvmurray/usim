//
//	picofram.h
//	Pico-Thing Persistent RAM (FRAM)
//
//	48 bytes at $FFD0-$FFFF, file-backed.
//	Includes 6809 hardware vectors at $FFF0-$FFFF (offsets 32-47).
//	Loaded from file on startup, saved on every write.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <cstring>
#include "device.h"

class PicoFRAM : public MappedDevice {

	static const size_t	FRAM_SIZE = 48;
	uint8_t			data[FRAM_SIZE];
	const char*		filepath;

public:
			PicoFRAM(const char* filepath);
	virtual		~PicoFRAM() = default;

	Byte		read(Word offset) override;
	void		write(Word offset, Byte val) override;

	void		load();
	void		save();
};
