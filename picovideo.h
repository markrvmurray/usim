//
//	picovideo.h
//	Pico-Thing video Pico at $FF10-$FF17
//
//	Iteration 1: the eight registers behave as plain RAM (write, read
//	back — no protocol yet), and the optional SDL window shows whatever
//	the pico-thing-video firmware's PORTABLE renderer (video_render.cpp,
//	compiled into this binary unchanged) has drawn — at reset, its
//	built-in test pattern. The register protocol (the MC6850-shaped
//	VID.Data/VID.Stat link) replaces the RAM behaviour in iteration 2.
//
//	The window is strictly opt-in (--video): scripted/batch runs must
//	never open one. Display service happens in tick() and costs zero
//	guest cycles, so attaching this device cannot drift cycle counts.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <cstring>
#include "device.h"
#include "usim.h"

class PicoVideo : public ActiveMappedDevice {

	static const size_t	NREGS = 8;
	static const uint32_t	SERVICE_INTERVAL = 20000;	// guest cycles (~50Hz at 1MHz)

	Byte			regs[NREGS];
	USim&			sys;
	bool			window;		// --video given
	int			scale;		// --video=N
	uint32_t		cycles = 0;

public:
			PicoVideo(USim& sys, bool window, int scale);
	virtual		~PicoVideo();

	void		reset() override;
	void		tick(uint32_t ticks) override;
	Byte		read(Word offset) override;
	void		write(Word offset, Byte val) override;
};
