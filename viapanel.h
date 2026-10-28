//
//	viapanel.h
//	A front panel for the VIA at $FF20: an LED and a switch on every
//	port pin, PA0-PA7 and PB0-PB7, and on CA1/CA2 and CB1/CB2, with an
//	LED for ~IRQ.
//
//	Each LED shows the level on its pin: red where the VIA drives the
//	pin, green where the pin is an input and its switch drives it.
//	Clicking a switch flips it; up is high. A switch on a pin the VIA
//	is driving is greyed out but keeps its setting, which reaches the
//	pin as soon as the guest makes it an input. CA1 and CB1 have push
//	buttons instead, pulling the line low while held, so a single
//	click gives the guest both edges.
//
//	The window is strictly opt-in (--via-panel) and its own SDL
//	window, beside --video's when both are open. Closing it leaves the
//	emulator running and the switches where they were; quitting the
//	application ends the run, as it does from the video window. Like
//	the video window it is serviced from tick() and costs no guest
//	cycles, so a click reaches the VIA at the next service, up to
//	SERVICE_INTERVAL cycles on.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <memory>
#include "device.h"
#include "mos6522.h"
#include "usim.h"

// SDL stays out of every TU but the panel's own
struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;

class ViaPanel : public ActiveDevice {

	static const uint32_t	SERVICE_INTERVAL = 20000;	// guest cycles (~50Hz at 1MHz)

	USim&			sys;
	std::shared_ptr<mos6522>	via;
	int			scale;
	uint32_t		cycles = 0;

	SDL_Window*		win = nullptr;
	SDL_Renderer*		ren = nullptr;
	bool			inited = false;	// sdlhost acquired
	bool			opened = false;	// opening attempted
	bool			closed = false;
	bool			quit = false;
	uint32_t		last_present_ms = 0;

	// the switches: port pins by bit, then C?1 and C?2
	Byte			sw_a = 0xff, sw_b = 0xff;
	bool			sw_ca1 = true, sw_ca2 = true;
	bool			sw_cb1 = true, sw_cb2 = true;
	int			held = -1;	// the push button down, by port

	bool			open_window();
	void			close_window();
	void			on_event(const SDL_Event& ev);
	void			click(int x, int y);
	void			release();
	void			apply();
	void			draw();

public:
			ViaPanel(USim& sys, std::shared_ptr<mos6522> via, int scale);
	virtual		~ViaPanel();

	void		reset() override;
	void		tick(uint32_t cycles) override;
};
