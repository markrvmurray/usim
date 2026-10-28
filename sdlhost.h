//
//	sdlhost.h
//	SDL for every window usim09pt opens: initialised once, however
//	many windows use it, and its one event queue shared out by window,
//	so each window sees only its own input. SDL_QUIT, which belongs to
//	no window, goes to all of them.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <SDL.h>
#include <functional>

using SdlHandler = std::function<void(const SDL_Event&)>;

// SDL_Init on first use; false if SDL cannot start
bool sdlhost_acquire();

// SDL_Quit once the last user has released it
void sdlhost_release();

// deliver this window's events to the handler, or stop delivering them
void sdlhost_listen(Uint32 window_id, SdlHandler handler);
void sdlhost_ignore(Uint32 window_id);

// deliver every queued event
void sdlhost_pump();
