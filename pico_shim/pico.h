//
//	pico_shim/pico.h
//	Host-build shim for pico-thing-video firmware TUs compiled into usim.
//
//	Maps the pico-sdk decorations the firmware sources use onto no-ops so
//	the SAME source files build for RP2350 (real SDK) and for the host
//	(this shim). Keep it minimal: only what the portable TUs actually use.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#define __scratch_y(group)
#define __not_in_flash_func(f) f
