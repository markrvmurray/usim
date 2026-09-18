//
//	picovideo_display.cpp
//	SDL window for the Pico-Thing video device (see picovideo_display.h)
//
//	vim: ts=8 sw=8 noet:
//

#include <SDL.h>

#include "picovideo_display.h"

// From the pico-thing-video firmware tree (portable renderer half).
#include "video.h"
#include "video_render.h"

static SDL_Window*	win;
static SDL_Renderer*	ren;
static SDL_Texture*	tex;
static bool		sdl_up;		// display_init attempted
static bool		inited;		// SDL_Init succeeded
static bool		closed;
static uint32_t		last_present_ms;
static uint32_t		rgb332_lut[256];

static const uint32_t	PRESENT_FLOOR_MS = 30;	// ~33 fps cap

// RGB332 -> ARGB8888, replicating high bits into low so 0xE0 maps to a
// full 0xFF channel (matches how the HSTX TMDS encoder treats the top
// bits as the significant ones).
static void build_lut()
{
	for (unsigned i = 0; i < 256; i++) {
		unsigned r3 = (i >> 5) & 7, g3 = (i >> 2) & 7, b2 = i & 3;
		unsigned r = (r3 << 5) | (r3 << 2) | (r3 >> 1);
		unsigned g = (g3 << 5) | (g3 << 2) | (g3 >> 1);
		unsigned b = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2;
		rgb332_lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
	}
}

static bool display_init(int scale)
{
	// Never fight the emulator's signal conventions: SIGINT stays
	// ignored, and `killall usim09pt` must keep working.
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "picovideo: SDL_Init failed: %s\n", SDL_GetError());
		return false;
	}
	inited = true;
	if (scale < 1) scale = 1;
	win = SDL_CreateWindow("pico-thing-video",
	    SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	    FB_WIDTH * scale, FB_HEIGHT * scale, 0);
	if (win == nullptr) {
		fprintf(stderr, "picovideo: SDL_CreateWindow failed: %s\n", SDL_GetError());
		return false;
	}
	ren = SDL_CreateRenderer(win, -1, 0);
	if (ren == nullptr) {
		fprintf(stderr, "picovideo: SDL_CreateRenderer failed: %s\n", SDL_GetError());
		return false;
	}
	SDL_RenderSetLogicalSize(ren, FB_WIDTH, FB_HEIGHT);
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
	    SDL_TEXTUREACCESS_STREAMING, FB_WIDTH, FB_HEIGHT);
	if (tex == nullptr) {
		fprintf(stderr, "picovideo: SDL_CreateTexture failed: %s\n", SDL_GetError());
		return false;
	}
	build_lut();
	return true;
}

bool picovideo_display_service(int scale)
{
	if (closed)
		return false;
	if (!sdl_up) {
		sdl_up = true;	// one attempt; failure downgrades to headless
		if (!display_init(scale)) {
			closed = true;
			return false;
		}
	}

	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		if (ev.type == SDL_QUIT) {
			closed = true;
			return false;
		}
	}

	// Wall-clock floor: the guest-cycle interval fires far faster than
	// 33 fps when the emulator free-runs.
	uint32_t now = SDL_GetTicks();
	if (now - last_present_ms < PRESENT_FLOOR_MS)
		return true;
	last_present_ms = now;

	void* pixels;
	int pitch;
	if (SDL_LockTexture(tex, nullptr, &pixels, &pitch) == 0) {
		for (int y = 0; y < FB_HEIGHT; y++) {
			const uint8_t* src = video_framebuffer[y];
			uint32_t* dst = (uint32_t*)((uint8_t*)pixels + y * pitch);
			for (int x = 0; x < FB_WIDTH; x++)
				dst[x] = rgb332_lut[src[x]];
		}
		SDL_UnlockTexture(tex);
	}
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, nullptr, nullptr);
	SDL_RenderPresent(ren);
	return true;
}

void picovideo_display_shutdown()
{
	if (tex != nullptr) { SDL_DestroyTexture(tex); tex = nullptr; }
	if (ren != nullptr) { SDL_DestroyRenderer(ren); ren = nullptr; }
	if (win != nullptr) { SDL_DestroyWindow(win); win = nullptr; }
	if (inited) { SDL_Quit(); inited = false; }
	sdl_up = false;
}

// The firmware's hardware TU (video.cpp) provides vidterm_present() on the
// board; portable code that calls it needs a host definition here.
extern "C" void vidterm_present(void)
{
}
