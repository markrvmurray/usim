//
//	test_picovideo_keys.cpp
//	The video window hands the guest the bytes the firmware's USB cooker
//	does: F1-F12 are $80-$8B. Runs the real SDL event path on SDL's dummy
//	video driver, so it needs no display.
//
//	vim: ts=8 sw=8 noet:
//

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "picovideo_display.h"
#include "vid_link.h"

static std::vector<uint8_t> got;

void vid_link_input(uint8_t c)
{
	got.push_back(c);
}

// picovideo.cpp's side of the link, which this test leaves out.
void vid_link_bell(void)
{
}

void vid_link_info_set(uint16_t, uint16_t, uint8_t, uint8_t)
{
}

void vid_link_tone(uint16_t, uint8_t, uint8_t)
{
}

static void key(SDL_Keycode k)
{
	SDL_Event ev{};
	ev.type = SDL_KEYDOWN;
	ev.key.keysym.sym = k;
	SDL_PushEvent(&ev);
	picovideo_display_service(1);
}

int main()
{
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	picovideo_display_service(1);
	int fails = 0;
	for (int i = 0; i < 12; i++) {
		got.clear();
		key(SDLK_F1 + i);
		if (got.size() != 1 || got[0] != 0x80 + i) {
			printf("FAIL F%d\n", i + 1);
			fails++;
		}
	}
	got.clear();
	key(SDLK_RETURN);
	if (got.size() != 1 || got[0] != 0x0D) {
		printf("FAIL return\n");
		fails++;
	}
	picovideo_display_shutdown();
	printf("test_picovideo_keys: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
