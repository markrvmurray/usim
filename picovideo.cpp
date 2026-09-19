//
//	picovideo.cpp
//	Pico-Thing video Pico at $FF10-$FF17 (see picovideo.h)
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>

#include "picovideo.h"
#include "picovideo_display.h"

// From the pico-thing-video firmware tree: the portable renderer half and
// the guest-link protocol (vid_link.h documents the register contract).
#include "video.h"
#include "video_render.h"
// Angle brackets on purpose: usim has its own registers.h (CPU state),
// and a quoted include would find it via the including file's directory.
// The Makefile puts $(PICOVIDEO)/include ahead of -I. so <> resolves to
// the firmware's register-window class.
#include <registers.h>
#include "vid_link.h"

extern registers &reg;	// the read-side proxy singleton (registers.cpp)

PicoVideo::PicoVideo(USim& sys, bool window, int scale,
    const char* shot_path)
	: sys(sys), window(window), scale(scale), shot_path(shot_path)
{
}

PicoVideo::~PicoVideo()
{
	// Capture before teardown: on a clean exit (halt or timeout) the
	// framebuffer still holds whatever the guest last drew.
	if (shot_path != nullptr) {
		if (picovideo_screenshot(shot_path))
			fprintf(stderr, "picovideo: screenshot written to %s\n", shot_path);
		else
			fprintf(stderr, "picovideo: screenshot to %s FAILED\n", shot_path);
	}
	picovideo_display_shutdown();
}

void PicoVideo::reset()
{
	// Power-on state of the video Pico: cleared screen, then the
	// firmware's own demo/test pattern — rendered by the identical
	// video_render.cpp the RP2350 runs.
	video_render_init();
	video_demo();
	vidterm_refresh();

	// Bring the guest link up exactly as Core 1's bring-up does:
	// window cleared, then READY + protocol version staged.
	registers::clear();
	vid_link_init();
}

void PicoVideo::set_input(const char* s, uint64_t first, uint64_t step)
{
	inject = s;
	inject_first = first;
	inject_step = step;
	inject_pos = 0;
}

void PicoVideo::tick(uint32_t ticks)
{
	// Cycle-scheduled keystroke injection (--video-input), released one
	// byte per step like the serial console's ScriptTerminal — through
	// the same vid_link_input() entry the real USB HID cooker uses.
	while (inject_pos < inject.size() &&
	    sys.get_total_cycles() >= inject_first + inject_pos * inject_step) {
		vid_link_input((uint8_t)inject[inject_pos]);
		inject_pos++;
	}

	// The Core 1 dispatch loop's role: drain the command FIFO, restage
	// Data/Stat. Runs headless too — the link must work without a window.
	vid_link_task();

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
	// Serve-then-pop, the same order as the hardware's read DMA + ISR:
	// the staged byte goes to the guest, then the link advances behind it.
	uint8_t val;

	registers::copy_out(&val, offset, 1);
	vid_link_guest_read((uint8_t)offset);
	return val;
}

void PicoVideo::write(Word offset, Byte val)
{
	// Replicate the write ISR: store to the write side, mirror the
	// reserved (scratch) offsets to the read side, dispatch to the link.
	registers::write((uint16_t)offset) = val;
	if (offset > VID_STAT_OFFSET)
		reg[(uint16_t)offset] = val;
	vid_link_guest_write((uint8_t)offset, val);
}
