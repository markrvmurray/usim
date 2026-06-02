//
//	system_watchpoint.cpp
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include "system_watchpoint.h"

//
// BREAK snippet, copied to $FFE0-$FFEF when the watchpoint is armed.
//
// 6809 NMI auto-pushes the full register frame onto the stack:
//   PC,U,Y,X,DP,B,A,CC  (high addr → low addr), so after entry S
//   points at CC and there are 12 bytes (CC,A,B,DP,X,Y,U,PC) to copy.
//
// Assembly equivalent (assembled to exactly 16 bytes):
//   FFE0: 30 E4         LEAX 0,S        ; X = pointer to NMI frame
//   FFE2: CE FF D0      LDU  #$FFD0     ; SHARED destination
//   FFE5: 86 0C         LDA  #12
//   FFE7: E6 80     loop: LDB  ,X+
//   FFE9: E7 C0         STB  ,U+
//   FFEB: 4A            DECA
//   FFEC: 26 F9         BNE  loop       ; -7
//   FFEE: 20 FE     spin: BRA  spin     ; -2
//
const Byte SystemWatchpoint::BREAK_SNIPPET[SystemWatchpoint::SNIPPET_LEN] = {
	0x30, 0xE4,			// LEAX 0,S
	0xCE, 0xFF, 0xD0,		// LDU  #$FFD0
	0x86, 0x0C,			// LDA  #12
	0xE6, 0x80,			// LDB  ,X+
	0xE7, 0xC0,			// STB  ,U+
	0x4A,				// DECA
	0x26, 0xF9,			// BNE  loop
	0x20, 0xFE			// BRA  spin
};

void SystemWatchpoint::reset()
{
	if (m_armed) {
		// Restore NMI vector before clearing armed state.
		const Word nmi_off = NMI_VEC_ADDR - WINDOW_BASE;
		store_shadow(nmi_off,     m_saved_nmi_hi);
		store_shadow(nmi_off + 1, m_saved_nmi_lo);
	}
	m_armed = false;
	m_nmi_request = false;
}

void SystemWatchpoint::load(Word abs_addr, Byte val)
{
	// 32-bit math on the upper bound — the watchpoint window reaches
	// $FFFF, so WINDOW_BASE + WINDOW_SIZE = $10000 would wrap to 0
	// as a Word and silently no-op every load.
	if (abs_addr >= WINDOW_BASE &&
	    (uint32_t)abs_addr < (uint32_t)WINDOW_BASE + (uint32_t)WINDOW_SIZE) {
		store_shadow((Word)(abs_addr - WINDOW_BASE), val);
	}
}

void SystemWatchpoint::set_backing(WatchpointBacking* backing)
{
	m_backing = backing;
	if (!backing) return;

	// Seed the shadow from the backing without re-storing each byte
	// back through it — bulk-mirror only, no write-back round trip.
	for (Word i = 0; i < WINDOW_SIZE; i++) {
		m_shadow[i] = backing->load_byte(i);
	}
}

void SystemWatchpoint::ctrl_write(Byte val)
{
	if (val) {
		arm();
	} else {
		disarm();
	}
}

Byte SystemWatchpoint::vec_read(Word offset_in_window) const
{
	if (offset_in_window < WINDOW_SIZE) {
		return m_shadow[offset_in_window];
	}
	return 0xFF;
}

void SystemWatchpoint::vec_write(Word offset_in_window, Byte val)
{
	if (offset_in_window >= WINDOW_SIZE) return;

	if (m_armed) {
		// Block the store; assert NMI; do NOT auto-disarm here.
		// The CPU is about to vector to the snippet at $FFE0 (the
		// swapped NMI vector points there); the snippet will copy
		// the register frame to $FFD0 and spin. Disarm only on
		// explicit write of 0 to $FFCA, or on cpu.reset().
		trigger((Word)(WINDOW_BASE + offset_in_window));
		return;
	}

	store_shadow(offset_in_window, val);
}

void SystemWatchpoint::arm()
{
	if (m_armed) return;	// idempotent

	// Save the current NMI vector from the shadow.
	const Word nmi_off = NMI_VEC_ADDR - WINDOW_BASE;
	m_saved_nmi_hi = m_shadow[nmi_off];
	m_saved_nmi_lo = m_shadow[nmi_off + 1];

	// Copy the BREAK snippet over $FFE0-$FFEF.
	for (Word i = 0; i < SNIPPET_LEN; i++) {
		store_shadow((Word)((SNIPPET_BASE - WINDOW_BASE) + i),
			     BREAK_SNIPPET[i]);
	}

	// Repoint the NMI vector to $FFE0.
	store_shadow(nmi_off,     (Byte)(SNIPPET_BASE >> 8));
	store_shadow(nmi_off + 1, (Byte)(SNIPPET_BASE & 0xFF));

	m_armed = true;
}

void SystemWatchpoint::disarm()
{
	if (!m_armed) return;

	// Restore the saved NMI vector. Snippet bytes are intentionally
	// left in place to match hardware semantics (the Pico's BREAK
	// snippet stays resident until the next firmware-load cycle).
	const Word nmi_off = NMI_VEC_ADDR - WINDOW_BASE;
	store_shadow(nmi_off,     m_saved_nmi_hi);
	store_shadow(nmi_off + 1, m_saved_nmi_lo);

	m_armed = false;
	m_nmi_request = false;	// release NMI
}

void SystemWatchpoint::trigger(Word offending_addr)
{
	// Visibility: emulator-side dump so users see the trap even
	// before the snippet copies the frame to SHARED. Doesn't affect
	// guest state.
	fprintf(stderr, "SystemWatchpoint: write to $%04X blocked while armed; "
		"asserting NMI (PC=$%04X)\n",
		(unsigned)offending_addr, (unsigned)cpu.get_pc());

	// Pulse NMI by setting m_nmi_request true. The CPU samples NMI
	// each tick and fires on the falling edge (`!c_nmi && nmi_previous`),
	// so this fires exactly once per arm; subsequent triggers within
	// the same armed window won't re-fire until disarm releases NMI
	// and a fresh arm + trigger occurs.
	m_nmi_request = true;
}
