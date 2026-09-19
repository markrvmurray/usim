//
//	pico_shim/hardware/pio.h — host stand-in (see ../pico.h).
//
//	pico_thing_video.h's pio_dma struct names the PIO handle type; give
//	the host an opaque equivalent.
//
#pragma once

typedef void* PIO;
