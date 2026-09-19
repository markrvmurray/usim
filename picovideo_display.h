//
//	picovideo_display.h
//	SDL window for the Pico-Thing video device — the ONLY TU that
//	touches SDL, so the dependency stays out of every other binary.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

// Service the window: lazily create it (scale x logical size), pump SDL
// events, and (rate-limited by a wall-clock floor) blit the firmware
// renderer's framebuffer. Returns false once the user closes the window.
bool picovideo_display_service(int scale);

// Tear the window and SDL down (idempotent; safe if never initialised).
void picovideo_display_shutdown();

// Write the firmware framebuffer as a PNG. zlib only — no SDL, no window;
// works in fully headless runs. Returns true on success.
bool picovideo_screenshot(const char* path);
