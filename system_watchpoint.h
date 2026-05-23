//
//	system_watchpoint.h
//	Pico-Thing-style SYSTEM_WATCHPOINT hardware-assisted vector breakpoint.
//
//	Hardware spec (matches the real pico-thing SBC, where the Pico
//	co-processor implements the device via DMA write trapping):
//
//	  - Control register at $FFCA. Write non-zero to arm, zero to
//	    disarm. Reads return 0.
//	  - On arm:
//	      1. Save the current NMI vector ($FFFC-$FFFD).
//	      2. Copy a 16-byte BREAK snippet to $FFE0-$FFEF.
//	      3. Set the NMI vector to $FFE0.
//	  - On a write to the protected window ($FFE0-$FFFF) while armed:
//	      - Block the store.
//	      - Assert NMI.
//	      The CPU services NMI, reads the (swapped) vector, jumps to
//	      $FFE0, and the snippet copies the 12-byte NMI stack frame
//	      (CC,A,B,DP,X,Y,U,PC) to SHARED ($FFD0-$FFDB), then spins.
//	  - On disarm (write 0 to $FFCA, or cpu.reset()):
//	      - Restore the saved NMI vector into the shadow.
//	      - Release the NMI line.
//	      - Snippet bytes left as-is in the shadow (matches hardware).
//
//	The device owns a 32-byte shadow buffer for $FFE0-$FFFF. Firmware
//	loaders should call `load(addr, val)` for each byte in that range
//	to seed the shadow before reset — that's how the reset vector
//	at $FFFE-$FFFF reaches the CPU.
//
//	SHARED area ($FFD0-$FFDB) is the snippet's destination. On a real
//	pico-thing it's writable FRAM. On usim09batch the snippet would
//	collide with the ACIA / HaltDevice / TraceCtl devices that sit at
//	$FFD0-$FFD3, so triggering the watchpoint on that driver is for
//	probing the *trap*, not for letting the snippet run to completion.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <cstring>
#include "device.h"
#include "wiring.h"
#include "mc6809.h"

class SystemWatchpoint : public ActiveDevice {

public:
	static constexpr Word	WINDOW_BASE	= 0xFFE0;
	static constexpr Word	WINDOW_SIZE	= 0x0020;
	static constexpr Word	CTRL_ADDR	= 0xFFCA;
	static constexpr Word	NMI_VEC_ADDR	= 0xFFFC;	// big-endian: hi byte
	static constexpr Word	SNIPPET_BASE	= 0xFFE0;
	static constexpr Word	SNIPPET_LEN	= 0x10;

private:
	mc6809&			cpu;
	bool			m_armed = false;
	bool			m_nmi_request = false;	// true = NMI asserted (active-low pin)
	Byte			m_shadow[WINDOW_SIZE];	// $FFE0-$FFFF
	Byte			m_saved_nmi_hi = 0xFF;
	Byte			m_saved_nmi_lo = 0xFF;

	static const Byte	BREAK_SNIPPET[SNIPPET_LEN];

public:
	// NMI is active-low. m_nmi_request=true → pin reads false.
	OutputPin		NMI;

				SystemWatchpoint(mc6809& cpu)
					: cpu(cpu), NMI(m_nmi_request, /*invert=*/true)
				{
					std::memset(m_shadow, 0xFF, sizeof(m_shadow));
				}

	// ActiveDevice — empty tick(); reset() disarms.
	void			tick(uint8_t /*cycles*/) override { }
	void			reset() override;

	// Loader-side: seed the vector shadow from a firmware image.
	void			load(Word abs_addr, Byte val);

	// Adapter-callable surfaces:
	Byte			ctrl_read() const { return 0; }
	void			ctrl_write(Byte val);
	Byte			vec_read(Word offset_in_window) const;
	void			vec_write(Word offset_in_window, Byte val);

	// Introspection (for tests).
	bool			armed() const { return m_armed; }
	bool			nmi_asserted() const { return m_nmi_request; }
	Byte			shadow_byte(Word offset_in_window) const {
					return (offset_in_window < WINDOW_SIZE)
						? m_shadow[offset_in_window] : 0xFF;
				}

private:
	void			arm();
	void			disarm();
	void			trigger(Word offending_addr);
};

//
// Adapter MappedDevice for the $FFCA control register.
//
class SystemWatchpointCtrl : public MappedDevice {
	SystemWatchpoint&	w;
public:
				SystemWatchpointCtrl(SystemWatchpoint& w) : w(w) {}
	Byte			read(Word /*offset*/) override { return w.ctrl_read(); }
	void			write(Word /*offset*/, Byte val) override { w.ctrl_write(val); }
};

//
// Adapter MappedDevice for the $FFE0-$FFFF vector + snippet window.
//
class SystemWatchpointVec : public MappedDevice {
	SystemWatchpoint&	w;
public:
				SystemWatchpointVec(SystemWatchpoint& w) : w(w) {}
	Byte			read(Word offset) override { return w.vec_read(offset); }
	void			write(Word offset, Byte val) override { w.vec_write(offset, val); }
};
