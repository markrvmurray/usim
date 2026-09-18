//
//	picovideo.cpp
//	Pico-Thing video Pico at $FF10-$FF17 (see picovideo.h)
//
//	vim: ts=8 sw=8 noet:
//

#include "picovideo.h"
#include "picovideo_display.h"

// From the pico-thing-video firmware tree (portable renderer half).
#include "video.h"
#include "video_render.h"

PicoVideo::PicoVideo(USim& sys, bool window, int scale)
	: sys(sys), window(window), scale(scale)
{
	memset(regs, 0, sizeof(regs));
}

PicoVideo::~PicoVideo()
{
	picovideo_display_shutdown();
}

void PicoVideo::reset()
{
	memset(regs, 0, sizeof(regs));

	// Power-on state of the video Pico: cleared screen, then the
	// firmware's own demo/test pattern — rendered by the identical
	// video_render.cpp the RP2350 runs.
	video_render_init();
	video_demo();
	vidterm_refresh();
}

void PicoVideo::tick(uint32_t ticks)
{
	if (!window)
		return;

	// mc6850-style rate limit: service the display every ~20000 guest
	// cycles. The display module applies its own wall-clock floor on
	// top, so a fast host doesn't spin the blit.
	cycles += ticks;
	if (cycles < SERVICE_INTERVAL)
		return;
	cycles = 0;

	if (!picovideo_display_service(scale))
		sys.halt();	// window closed (interactive runs only)
}

Byte PicoVideo::read(Word offset)
{
	return regs[offset % NREGS];
}

void PicoVideo::write(Word offset, Byte val)
{
	regs[offset % NREGS] = val;
}
