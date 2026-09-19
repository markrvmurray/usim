//
//	picovideo.h
//	Pico-Thing video Pico at $FF10-$FF17
//
//	The registers speak the firmware's own guest-link protocol: the
//	MC6850-shaped VID.Data/VID.Stat mailbox, served by the REAL
//	vid_link.cpp + registers.cpp compiled unchanged from the
//	pico-thing-video tree — reads pop the input FIFO exactly as the
//	board's bus-read ISR does, writes replicate its write ISR (store,
//	mirror for the reserved offsets, dispatch). The optional SDL window
//	shows whatever the firmware's PORTABLE renderer has drawn — at
//	reset, its built-in test pattern.
//
//	The window is strictly opt-in (--video): scripted/batch runs must
//	never open one. --screenshot=FILE writes the framebuffer as a PNG
//	when the emulator exits cleanly (halt or --timeout — not a signal),
//	window or no window, so scripted runs can capture what the guest
//	drew. Display service happens in tick() and costs zero
//	guest cycles, so attaching this device cannot drift cycle counts.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <cstring>
#include <string>
#include "device.h"
#include "usim.h"

class PicoVideo : public ActiveMappedDevice {

	static const uint32_t	SERVICE_INTERVAL = 20000;	// guest cycles (~50Hz at 1MHz)

	USim&			sys;
	bool			window;		// --video given
	int			scale;		// --video=N
	const char*		shot_path;	// --screenshot=FILE (or null)
	uint32_t		cycles = 0;

	// --video-input: cycle-scheduled keystroke injection into the
	// guest-input FIFO (the sim-side equivalent of typing on the USB
	// keyboard), mirroring --input's scheduling for the serial console.
	std::string		inject;
	uint64_t		inject_first = 0;
	uint64_t		inject_step = 0;
	size_t			inject_pos = 0;

public:
			PicoVideo(USim& sys, bool window, int scale,
			    const char* shot_path);

	void		set_input(const char* s, uint64_t first, uint64_t step);
	virtual		~PicoVideo();

	void		reset() override;
	void		tick(uint32_t ticks) override;
	Byte		read(Word offset) override;
	void		write(Word offset, Byte val) override;
};
