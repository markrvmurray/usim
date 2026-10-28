//
//	sdlhost.cpp
//	SDL shared between usim09pt's windows (see sdlhost.h)
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <map>

#include "sdlhost.h"

static int				users;
static std::map<Uint32, SdlHandler>	listeners;

bool sdlhost_acquire()
{
	if (users > 0) {
		users++;
		return true;
	}

	// Never fight the emulator's signal conventions: SIGINT stays
	// ignored, and `killall usim09pt` must keep working.
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "sdlhost: SDL_Init failed: %s\n", SDL_GetError());
		return false;
	}
	users = 1;
	return true;
}

void sdlhost_release()
{
	if (users > 0 && --users == 0)
		SDL_Quit();
}

void sdlhost_listen(Uint32 window_id, SdlHandler handler)
{
	listeners[window_id] = std::move(handler);
}

void sdlhost_ignore(Uint32 window_id)
{
	listeners.erase(window_id);
}

static Uint32 window_of(const SDL_Event& ev)
{
	switch (ev.type) {
	case SDL_WINDOWEVENT:		return ev.window.windowID;
	case SDL_KEYDOWN:
	case SDL_KEYUP:			return ev.key.windowID;
	case SDL_TEXTINPUT:		return ev.text.windowID;
	case SDL_MOUSEMOTION:		return ev.motion.windowID;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:		return ev.button.windowID;
	case SDL_MOUSEWHEEL:		return ev.wheel.windowID;
	default:			return 0;
	}
}

void sdlhost_pump()
{
	SDL_Event ev;

	// A handler may close its window, and so stop listening, while its
	// event is being delivered: call a copy, never the map's entry.
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_QUIT) {
			auto all = listeners;
			for (auto& [id, handler] : all)
				handler(ev);
			continue;
		}
		auto it = listeners.find(window_of(ev));
		if (it == listeners.end())
			continue;
		auto handler = it->second;
		handler(ev);
	}
}
