//
//	test_picovideo.cpp
//	Unit tests for the PicoVideo guest link: the MC6850-shaped
//	VID.Data/VID.Stat protocol served by the pico-thing-video
//	firmware's own vid_link.cpp. Exercises the M0 contract the
//	NitrOS-9 side codes against: READY/version staging, TDRE flow
//	control, control-register loopback, pop-on-read ordering, the
//	scratch behaviour of the reserved registers, and soft reset.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <unistd.h>
#include <initializer_list>
#include <string>

#include "mc6809.h"
#include "picovideo.h"
#include "hid_cook.h"
#include "vid_link.h"
#include "video.h"
#include "video_render.h"
#include "gfx_shadow.h"
#include "cmd_trace.h"
#include "fill_log.h"
#include "gfx_bufs.h"
#include "win_store.h"
#include "pv_psram.h"
#include "font_spleen_8x16.h"

static int failures = 0;

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
				__FILE__, __LINE__, #cond);		\
			++failures;					\
		}							\
	} while (0)

// Headless: satisfy picovideo.cpp's display references without SDL.
bool picovideo_display_service(int)
{
	return true;
}

void picovideo_display_shutdown()
{
}

bool picovideo_screenshot(const char*)
{
	return true;
}

static const Word DATA = 0;	// $FF10
static const Word STAT = 1;	// $FF11

// The console renders white (0xFF) glyph pixels on an opaque black
// (0x00) background. Compare a text cell against the font directly —
// the same table the renderer blits from.
// Run render passes until the device has caught up. Both the command drain
// and the text refresh are BOUNDED per pass (a full screen of text can cost
// more than a frame, so it is spread over several), so one tick no longer
// means "done" -- and most checks here mean exactly "done". Those use this;
// the tests that measure a single pass on purpose (the drain bound, the
// refresh bound) still call tick(1). Bounded, and loud when the bound is hit:
// a device that never settles is a hang, and a silent loop would hide it.
static void settle(PicoVideo &v)
{
	for (int i = 0; i < 256; i++) {
		v.tick(1);
		if (vid_link_idle())
			return;
	}
	fprintf(stderr, "FAIL settle: render side still busy after 256 passes\n");
	failures++;
}

static bool check_cell(int col, int row, uint8_t ch)
{
	// The cursor lives in the framebuffer and inverts the cell it sits on,
	// so lift it first: these checks are about text content, not the
	// overlay. Cursor behaviour itself is asserted against the framebuffer
	// directly, after an explicit vidterm_cursor_service().
	vidterm_cursor_hide();

	for (int gy = 0; gy < CHAR_H; gy++) {
		uint8_t mask = font_spleen_8x16[ch][gy];
		for (int gx = 0; gx < CHAR_W; gx++) {
			uint8_t want = (mask & (0x80 >> gx)) ? 0xFF : 0x00;
			if (video_framebuffer[row * CHAR_H + gy][col * CHAR_W + gx] != want)
				return false;
		}
	}
	return true;
}

int main()
{
	mc6809 cpu;
	PicoVideo v(cpu, false, 1, nullptr);

	v.reset();

	// Power-on status: READY + TDRE + version 1, no input pending.
	Byte st = v.read(STAT);
	CHECK(st & VID_STAT_READY);
	CHECK(st & VID_STAT_TDRE);
	CHECK(!(st & VID_STAT_RDRF));
	CHECK(((st >> VID_STAT_VER_LSB) & 0x07) == VID_STAT_VERSION);

	// Without loopback, command bytes are consumed and vanish.
	v.write(DATA, 'x');
	settle(v);
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));

	// Loopback on: bytes round-trip, in order, popped by Data reads.
	v.write(STAT, VID_CTL_LOOPBACK);
	v.write(DATA, 'X');
	v.write(DATA, 'Y');
	settle(v);
	CHECK(v.read(STAT) & VID_STAT_RDRF);
	CHECK(v.read(DATA) == 'X');
	CHECK(v.read(STAT) & VID_STAT_RDRF);
	CHECK(v.read(DATA) == 'Y');
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));

	// The guest discipline (one Data read per RDRF check) is the
	// contract, but the pop must still advance even on an undisciplined
	// double read: with two bytes queued the second read returns the
	// second byte because the pop restages immediately (ISR-equivalent).
	v.write(DATA, 'A');
	v.write(DATA, 'B');
	settle(v);
	CHECK(v.read(DATA) == 'A');
	CHECK(v.read(DATA) == 'B');

	// Reserved registers are scratch: written value reads back.
	v.write(5, 0x5A);
	CHECK(v.read(5) == 0x5A);
	// ...and a Data write must NOT clobber the staged input byte.
	v.write(DATA, 'Z');
	settle(v);
	CHECK(v.read(STAT) & VID_STAT_RDRF);
	v.write(DATA, 0x00);	// enqueue another command byte
	CHECK(v.read(DATA) == 'Z');

	// Soft reset flushes both FIFOs (loopback bit carried in the same
	// write survives), and READY/version restage.
	v.write(DATA, 'Q');
	v.write(STAT, VID_CTL_RESET | VID_CTL_LOOPBACK);
	settle(v);
	st = v.read(STAT);
	CHECK(!(st & VID_STAT_RDRF));
	CHECK(st & VID_STAT_READY);
	CHECK(st & VID_STAT_TDRE);

	// Flow control: TDRE stays set through a long burst (2 KB FIFO,
	// drained per tick), and the drained bytes loop back in order.
	v.write(STAT, VID_CTL_RESET | VID_CTL_LOOPBACK);
	for (int i = 0; i < 256; i++) {
		CHECK(v.read(STAT) & VID_STAT_TDRE);
		v.write(DATA, (Byte)i);
	}
	settle(v);
	for (int i = 0; i < 256; i++) {
		CHECK(v.read(STAT) & VID_STAT_RDRF);
		CHECK(v.read(DATA) == (Byte)i);
	}
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));

	// ---- M1: the glass TTY (loopback OFF routes bytes to the console) ----

	// Reset re-draws the demo; the first console byte lazily claims the
	// screen: graphics cleared, text cleared, cursor homed.
	v.reset();
	v.write(STAT, 0x00);		// loopback off
	v.write(DATA, 'H');
	v.write(DATA, 'i');
	settle(v);
	CHECK(check_cell(0, 0, 'H'));
	CHECK(check_cell(1, 0, 'i'));
	CHECK(video_framebuffer[300][700] == 0x00);	// demo gone

	// CR returns to column 0 without a line feed: overwrite in place.
	v.write(DATA, 0x0D);
	v.write(DATA, 'X');
	settle(v);
	CHECK(check_cell(0, 0, 'X'));

	// LF moves down, keeping the column.
	v.write(DATA, 0x0A);
	v.write(DATA, 'Y');
	settle(v);
	CHECK(check_cell(1, 1, 'Y'));

	// Console bytes must not appear on the input FIFO when loopback is
	// off (they went to the renderer instead).
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));

	// Scrolling: line-feed to the bottom and once more; everything moves
	// up one row, so Y (row 1) lands on row 0 and X scrolls with it.
	for (int i = 0; i < 28; i++)
		v.write(DATA, 0x0A);	// row 1 + 28 = the bottom row
	settle(v);
	CHECK(check_cell(1, 1, 'Y'));	// not yet scrolled
	v.write(DATA, 0x0A);		// one beyond the bottom: scroll
	settle(v);
	CHECK(check_cell(1, 0, 'Y'));
	CHECK(!check_cell(0, 0, 'X'));	// X's row scrolled away... to row -1
	CHECK(check_cell(0, -1 + 1, 'X') == false);

	// ---- M2: the control-code table ----

	// Clear/home ($0C): wipes the text, next glyph lands at the origin.
	v.write(DATA, 0x0C);
	v.write(DATA, 'A');
	settle(v);
	CHECK(check_cell(0, 0, 'A'));
	CHECK(check_cell(1, 0, ' '));	// scrolled content gone

	// Cursor-XY ($02 col+$20 row+$20).
	v.write(DATA, 0x02);
	v.write(DATA, 0x20 + 10);
	v.write(DATA, 0x20 + 5);
	v.write(DATA, 'Q');
	settle(v);
	CHECK(check_cell(10, 5, 'Q'));

	// Home ($01).
	v.write(DATA, 0x01);
	v.write(DATA, 'H');
	settle(v);
	CHECK(check_cell(0, 0, 'H'));

	// Backspace ($08) is non-destructive cursor-left: "AB" BS 'C' -> "AC".
	v.write(DATA, 0x0C);
	v.write(DATA, 'A');
	v.write(DATA, 'B');
	v.write(DATA, 0x08);
	v.write(DATA, 'C');
	settle(v);
	CHECK(check_cell(0, 0, 'A'));
	CHECK(check_cell(1, 0, 'C'));

	// Erase to end of line ($04): position mid-text, tail clears, head stays.
	v.write(DATA, 0x0C);
	for (const char* s = "ABCDEF"; *s; s++)
		v.write(DATA, (Byte)*s);
	v.write(DATA, 0x02);
	v.write(DATA, 0x20 + 2);
	v.write(DATA, 0x20 + 0);
	v.write(DATA, 0x04);
	settle(v);
	CHECK(check_cell(1, 0, 'B'));
	CHECK(check_cell(2, 0, ' '));
	CHECK(check_cell(5, 0, ' '));

	// Erase line ($03): whole row blank, column preserved.
	v.write(DATA, 0x03);
	v.write(DATA, 'Z');
	settle(v);
	CHECK(check_cell(0, 0, ' '));	// 'A' erased too
	CHECK(check_cell(2, 0, 'Z'));	// column was preserved

	// Cursor right ($06) skips a cell; cursor up ($09) climbs a row.
	v.write(DATA, 0x0C);
	v.write(DATA, 'A');
	v.write(DATA, 0x06);
	v.write(DATA, 'B');
	settle(v);
	CHECK(check_cell(0, 0, 'A'));
	CHECK(check_cell(1, 0, ' '));
	CHECK(check_cell(2, 0, 'B'));
	v.write(DATA, 0x02);
	v.write(DATA, 0x20 + 3);
	v.write(DATA, 0x20 + 4);
	v.write(DATA, 0x09);
	v.write(DATA, 'U');
	settle(v);
	CHECK(check_cell(3, 3, 'U'));

	// Cursor on/off ($05 $21/$20): argument consumed, not printed.
	v.write(DATA, 0x0C);
	v.write(DATA, 'A');
	v.write(DATA, 0x05);
	v.write(DATA, 0x20);
	v.write(DATA, 'B');
	settle(v);
	CHECK(check_cell(1, 0, 'B'));

	// Bell ($07) is a no-op on this device.
	v.write(DATA, 0x07);
	v.write(DATA, 'C');
	settle(v);
	CHECK(check_cell(2, 0, 'C'));

	// Erase to end of window ($0B): below and right clear, above stays.
	v.write(DATA, 0x0C);
	v.write(DATA, 'T');			// top-left marker
	v.write(DATA, 0x02);
	v.write(DATA, 0x20 + 0);
	v.write(DATA, 0x20 + 10);
	for (const char* s = "MID"; *s; s++)
		v.write(DATA, (Byte)*s);
	v.write(DATA, 0x02);
	v.write(DATA, 0x20 + 0);
	v.write(DATA, 0x20 + 20);
	v.write(DATA, 'L');			// low marker
	v.write(DATA, 0x02);
	v.write(DATA, 0x20 + 1);
	v.write(DATA, 0x20 + 10);
	v.write(DATA, 0x0B);
	settle(v);
	CHECK(check_cell(0, 0, 'T'));		// above: intact
	CHECK(check_cell(0, 10, 'M'));		// left of cursor: intact
	CHECK(check_cell(1, 10, ' '));		// from cursor: erased
	CHECK(check_cell(2, 10, ' '));
	CHECK(check_cell(0, 20, ' '));		// rows below: erased

	// ---- M3: the $1B graphics dispatcher ----

	auto esc = [&](std::initializer_list<int> bytes) {
		v.write(DATA, 0x1B);
		for (int b : bytes)
			v.write(DATA, (Byte)b);
	};

	// Point with the default palette (FColor=7 -> white 0xFF).
	v.write(DATA, 0x0C);			// clean screen
	esc({0x42, 0, 200, 0, 100});		// Point (200,100)
	settle(v);
	CHECK(video_framebuffer[100][200] == 0xFF);

	// Palette + FColor: register 1 := GIME 36 (full red) -> 0xE0.
	esc({0x31, 1, 36});			// Palette reg1, colourValue 36
	esc({0x32, 1});				// FColor 1
	esc({0x42, 0, 210, 0, 100});		// Point (210,100)
	settle(v);
	CHECK(video_framebuffer[100][210] == 0xE0);

	// SetDPtr + LineM: a diagonal, and the pointer must follow.
	esc({0x32, 7});				// FColor back to white
	esc({0x40, 0, 50, 0, 200});		// SetDPtr (50,200)
	esc({0x46, 0, 60, 0, 210});		// LineM to (60,210)
	settle(v);
	for (int k = 0; k <= 10; k++)
		CHECK(video_framebuffer[200 + k][50 + k] == 0xFF);
	esc({0x4B, 0, 3, 0, 3});		// RBar +3,+3 from the MOVED pointer
	settle(v);
	CHECK(video_framebuffer[211][61] == 0xFF);	// inside (60..63,210..213)
	CHECK(video_framebuffer[213][63] == 0xFF);

	// Bar (absolute) fills the normalised rectangle.
	esc({0x40, 1, 44, 0, 150});		// SetDPtr (300,150)
	esc({0x4A, 1, 34, 0, 140});		// Bar to (290,140) — inverted corners
	settle(v);
	CHECK(video_framebuffer[140][290] == 0xFF);
	CHECK(video_framebuffer[150][300] == 0xFF);
	CHECK(video_framebuffer[145][295] == 0xFF);
	CHECK(video_framebuffer[139][290] == 0x00);	// outside

	// Framing: an implemented-later command (Circle, 2 params) is
	// consumed; the next byte prints as a glyph at the text cursor.
	v.write(DATA, 0x0C);
	esc({0x50, 30, 30});			// Circle — ignored, framed
	v.write(DATA, 'A');
	settle(v);
	CHECK(check_cell(0, 0, 'A'));

	// Framing: GPLoad's variable payload is swallowed — printable bytes
	// inside the payload must NOT render.
	esc({0x2B, 0, 1, 0, 0, 8, 0, 8, 0, 4});	// header: bufSize = 4
	v.write(DATA, 'W');			// payload...
	v.write(DATA, 'X');
	v.write(DATA, 'Y');
	v.write(DATA, 'Z');			// ...last payload byte
	v.write(DATA, 'B');			// first REAL byte again
	settle(v);
	CHECK(check_cell(1, 0, 'B'));
	CHECK(check_cell(2, 0, ' '));

	// Framing: a reserved sub-code is dropped without params. $27, not
	// $26 -- $26 became DWShow and now takes one, which is precisely why
	// a parameter count is not a free thing to change: this check used to
	// pass with $26 and started swallowing the byte after it.
	esc({0x27});
	v.write(DATA, 'C');
	settle(v);
	CHECK(check_cell(2, 0, 'C'));

	// ---- M4: guest input path (the USB keyboard's entry point) ----

	// vid_link_input feeds the RX FIFO exactly as the HID cooker does:
	// RDRF rises, reads pop in order, RDRF clears when drained.
	v.write(STAT, 0x00);			// loopback off
	settle(v);
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));
	vid_link_input('l');
	vid_link_input('s');
	vid_link_input(0x0D);
	settle(v);
	CHECK(v.read(STAT) & VID_STAT_RDRF);
	CHECK(v.read(DATA) == 'l');
	CHECK(v.read(DATA) == 's');
	CHECK(v.read(DATA) == 0x0D);
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));

	// Input bytes are for the guest only — they must not render.
	v.write(DATA, 0x0C);
	vid_link_input('k');
	settle(v);
	CHECK(check_cell(0, 0, ' '));
	CHECK(v.read(DATA) == 'k');

	// ---- Cursor: drawn into the framebuffer, with save-under ----

	// A console byte claims the screen and enables the cursor. (An earlier
	// case above turned the cursor off, so re-enable it explicitly rather
	// than inheriting that state.)
	v.write(DATA, 0x0C);
	v.write(DATA, 0x05);
	v.write(DATA, 0x21);
	v.write(DATA, 'A');
	settle(v);
	CHECK(video_cursor_enabled);
	CHECK(video_cursor_col == 1 && video_cursor_row == 0);  // parked after 'A'

	// The service loop draws it: the cell it sits on is inverted, and the
	// glyph to its left is untouched.
	vidterm_cursor_service();
	CHECK(video_framebuffer[4][CHAR_W + 0] == 0xFF);        // blank cell, inverted
	CHECK(check_cell(0, 0, 'A'));                           // 'A' unaffected

	// Hiding puts the covered pixels back exactly.
	vidterm_cursor_hide();
	CHECK(check_cell(1, 0, ' '));

	// It must not corrupt a glyph drawn where it was sitting: draw the
	// cursor, then let the console write over that very cell.
	vidterm_cursor_service();
	v.write(DATA, 'B');
	settle(v);
	vidterm_cursor_hide();
	CHECK(check_cell(1, 0, 'B'));   // no stale save-under painted over it

	// Turning the cursor off removes it from the framebuffer immediately.
	vidterm_cursor_service();
	v.write(DATA, 0x05);
	v.write(DATA, 0x20);
	settle(v);
	CHECK(!video_cursor_enabled);
	CHECK(check_cell(2, 0, ' '));   // where it had parked, now clean
	v.write(DATA, 0x05);
	v.write(DATA, 0x21);
	settle(v);

	// Blink: half a cycle later it is gone, a full cycle later it is back.
	vidterm_cursor_service();
	const int cx = video_cursor_col * CHAR_W;
	CHECK(video_framebuffer[4][cx] == 0xFF);
	video_frames += VIDEO_CURSOR_BLINK_FRAMES;
	vidterm_cursor_service();
	CHECK(video_framebuffer[4][cx] == 0x00);
	video_frames += VIDEO_CURSOR_BLINK_FRAMES;
	vidterm_cursor_service();
	CHECK(video_framebuffer[4][cx] == 0xFF);
	vidterm_cursor_hide();

	// ---- Mouse: polled snapshot on $FF12, change flag in Stat bit6 ----
	{
		const Word MOUSE = 2;   // $FF12

		// Drain whatever the boot state left staged, so the next pass
		// starts at byte 0 of a fresh latch.
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			(void)v.read(MOUSE);

		vid_link_mouse_update(300, 200, VID_MOUSE_BTN_LEFT, 0);
		settle(v);
		CHECK(v.read(STAT) & VID_STAT_MOUSE);   // something moved

		Byte s[VID_MOUSE_SNAPSHOT_LEN];
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(s[0] == VID_MOUSE_BTN_LEFT);
		CHECK(s[1] == VID_MOUSE_BTN_LEFT);          // press recorded
		CHECK(((s[2] << 8) | s[3]) == 300);         // X, big-endian
		CHECK(((s[4] << 8) | s[5]) == 200);         // Y
		CHECK((int8_t)s[6] == 0);

		// Reading the snapshot answers the change flag.
		settle(v);
		CHECK(!(v.read(STAT) & VID_STAT_MOUSE));

		// Sticky and wheel are cleared by the latch, so a second pass
		// over an unchanged mouse reports no new press.
		vid_link_mouse_update(300, 200, VID_MOUSE_BTN_LEFT, 0);
		settle(v);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(s[0] == VID_MOUSE_BTN_LEFT);          // still held
		CHECK(s[1] == 0);                           // but not newly pressed

		// THE POINT OF BYTE 1: a click entirely between two polls must
		// still be seen. Press and release with no read in between.
		vid_link_mouse_update(300, 200, 0, 0);              // release
		vid_link_mouse_update(300, 200, VID_MOUSE_BTN_RIGHT, 0);
		vid_link_mouse_update(300, 200, 0, 0);              // released again
		settle(v);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(s[0] == 0);                           // up by the time we look
		CHECK(s[1] & VID_MOUSE_BTN_RIGHT);          // yet the press survived

		// Wheel accumulates between latches and clears after one.
		vid_link_mouse_update(300, 200, 0, 2);
		vid_link_mouse_update(300, 200, 0, -1);
		settle(v);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK((int8_t)s[6] == 1);
		vid_link_mouse_update(300, 200, 0, 0);
		settle(v);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK((int8_t)s[6] == 0);

		// Writing $FF12 must not disturb the snapshot (no scratch mirror).
		v.write(MOUSE, 0x5A);
		settle(v);
		CHECK(v.read(MOUSE) != 0x5A);

		// RESYNC. A guest that loses count cannot detect it -- every
		// byte is an arbitrary value -- so it must be able to regain
		// phase unconditionally and cheaply. A write to $FF12 homes the
		// index, WITHOUT the link reset's collateral damage to the
		// command and input FIFOs.
		vid_link_mouse_update(111, 222, 0, 0);
		settle(v);
		(void)v.read(MOUSE);            // byte 0, then give up
		(void)v.read(MOUSE);            // byte 1; now out of phase
		v.write(MOUSE, 0x00);           // resync, no tick in between
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(((s[2] << 8) | s[3]) == 111);     // reading byte 0 again
		CHECK(((s[4] << 8) | s[5]) == 222);

		// The resync must not need a tick to take effect: on hardware
		// the guest can read $FF12 in the instruction after the write,
		// which would race Core 1's drain.
		(void)v.read(MOUSE);
		(void)v.read(MOUSE);
		(void)v.read(MOUSE);
		v.write(MOUSE, 0xFF);           // any value will do
		CHECK(v.read(MOUSE) == 0);      // byte 0: buttons, none held

		// The write TAKES A SAMPLE, and that sample is the guest's until
		// it reads it: later motion must not rewrite the pass underneath
		// the guest.
		vid_link_mouse_update(400, 300, 0, 0);
		v.write(MOUSE, 0x00);           // sample here
		vid_link_mouse_update(500, 350, 0, 0);  // moves on afterwards
		settle(v);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(((s[2] << 8) | s[3]) == 400);     // the sample, not the move
		CHECK(((s[4] << 8) | s[5]) == 300);

		// A soft reset also homes the index (belt and braces).
		vid_link_mouse_update(111, 222, 0, 0);
		settle(v);
		(void)v.read(MOUSE);
		v.write(STAT, 0x01);            // Ctl: soft reset
		settle(v);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(((s[2] << 8) | s[3]) == 111);
	}

	// ---- Pointer: drawn by the firmware, save-under restores ----
	{
		v.write(DATA, 0x0C);            // clean screen
		settle(v);
		vidterm_pointer_set(400, 300);
		vidterm_cursor_service();
		// Row 7 of the arrow is solid fill; the tip itself is outline,
		// which is drawn black and so invisible on a cleared screen.
		CHECK(video_framebuffer[300 + 7][400] == 0xFF);
		vidterm_cursor_hide();
		CHECK(video_framebuffer[300 + 7][400] == 0x00); // put back exactly

		// The guest can place it too (PutGC = $1B $4E, X then Y).
		v.write(DATA, 0x1B); v.write(DATA, 0x4E);
		v.write(DATA, 0x00); v.write(DATA, 0x64);       // X = 100
		v.write(DATA, 0x00); v.write(DATA, 0x32);       // Y = 50
		settle(v);
		vidterm_cursor_service();
		CHECK(video_framebuffer[50 + 7][100] == 0xFF);
		vidterm_cursor_hide();
	}

	// ---- HID cooking: synthetic boot reports drive the guest path ----
	// This is the code that had no coverage at all until the split, and
	// the reason it had none was that it could not compile host-side.
	{
		const Word MOUSE = 2;   // $FF12
		hid_cook_reset();

		// -- Mouse: report LENGTH. A boot mouse sends three bytes; wheel
		// and pan are optional extras. Requiring the full struct size
		// discarded every report from an ordinary mouse on real
		// hardware, which is the defect this test exists to prevent.
		const int cx0 = hid_cook_cursor_x(), cy0 = hid_cook_cursor_y();
		const uint8_t three[] = { 0x00, 10, (uint8_t)(int8_t)-5 };
		hid_cook_mouse(three, 3);
		CHECK(hid_cook_cursor_x() == cx0 + 10);
		CHECK(hid_cook_cursor_y() == cy0 - 5);

		const uint8_t four[] = { 0x00, 0, 0, 1 };        // + wheel
		hid_cook_mouse(four, 4);
		const uint8_t five[] = { 0x00, 0, 0, 0, 0 };     // + pan
		hid_cook_mouse(five, 5);
		CHECK(hid_cook_cursor_x() == cx0 + 10);         // no spurious motion

		// Too short is ignored rather than read past the end.
		const uint8_t two[] = { 0x00, 99 };
		hid_cook_mouse(two, 2);
		CHECK(hid_cook_cursor_x() == cx0 + 10);

		// -- Mouse: deltas are SIGNED, and the cursor clamps to screen.
		const uint8_t hard_left[] = { 0x00, (uint8_t)(int8_t)-128, 0 };
		for (int i = 0; i < 20; i++)
			hid_cook_mouse(hard_left, 3);
		CHECK(hid_cook_cursor_x() == 0);                // clamped, not wrapped

		// -- Mouse: buttons reach the guest snapshot, through the link.
		hid_cook_reset();
		const uint8_t left_down[] = { 0x01, 0, 0 };
		hid_cook_mouse(left_down, 3);
		settle(v);
		v.write(MOUSE, 0);                              // sample and home
		Byte s[VID_MOUSE_SNAPSHOT_LEN];
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(s[0] == VID_MOUSE_BTN_LEFT);
		CHECK(((s[2] << 8) | s[3]) == hid_cook_cursor_x());
		CHECK(((s[4] << 8) | s[5]) == hid_cook_cursor_y());

		// -- Keyboard: a keystroke becomes a guest input byte. Reports
		// are modifier, reserved, then up to six held usages.
		auto kbd = [&](uint8_t mod, uint8_t usage) {
			const uint8_t r[8] = { mod, 0, usage, 0, 0, 0, 0, 0 };
			hid_cook_keyboard(r, 8);
		};
		auto released = [&]() {
			const uint8_t r[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
			hid_cook_keyboard(r, 8);
		};
		auto typed = [&]() -> int {
			settle(v);
			if (!(v.read(STAT) & VID_STAT_RDRF))
				return -1;
			return v.read(DATA);
		};

		hid_cook_reset();
		v.write(STAT, VID_CTL_RESET | VID_CTL_LOOPBACK); // divert to input
		v.write(STAT, VID_CTL_LOOPBACK);
		kbd(0x00, 0x04); released();                    // 'a'
		CHECK(typed() == 'a');
		kbd(0x02, 0x04); released();                    // shift -> 'A'
		CHECK(typed() == 'A');

		// A key HELD across reports must not repeat: the second report
		// still lists it, and only new-downs produce a byte.
		kbd(0x00, 0x04);
		CHECK(typed() == 'a');
		kbd(0x00, 0x04);                                // still held
		CHECK(typed() == -1);
		released();

		// UK layout, the part that differs from US and was never tested.
		kbd(0x02, 0x1F); released(); CHECK(typed() == '"');   // shift-2
		kbd(0x02, 0x20); released(); CHECK(typed() == 0x9C);  // shift-3 = GBP
		kbd(0x02, 0x34); released(); CHECK(typed() == '@');   // shift-'
		kbd(0x00, 0x31); released(); CHECK(typed() == '#');   // hash key
		kbd(0x02, 0x31); released(); CHECK(typed() == '~');
		kbd(0x02, 0x35); released(); CHECK(typed() == 0xAA);  // shift-` = not
		kbd(0x00, 0x64); released(); CHECK(typed() == '\\');  // ISO backslash

		// Enter is CR, not LF: OS-9's line terminator, and the guest
		// input contract. This was found by reading the code, not by
		// running it -- exactly the gap the split closes.
		kbd(0x00, 0x28); released(); CHECK(typed() == 0x0D);

		// Ctrl chords fold onto the C0 controls the shell edits with.
		kbd(0x01, 0x04); released(); CHECK(typed() == 0x01);  // ctrl-A
		kbd(0x01, 0x06); released(); CHECK(typed() == 0x03);  // ctrl-C

		// Caps lock is a toggle on press, and XORs with shift.
		kbd(0x00, 0x39); released();                    // caps on
		CHECK(hid_cook_caps_lock());
		kbd(0x00, 0x04); released(); CHECK(typed() == 'A');
		kbd(0x02, 0x04); released(); CHECK(typed() == 'a');   // shift undoes it
		kbd(0x00, 0x39); released();                    // caps off
		CHECK(!hid_cook_caps_lock());

		// Function keys reach the guest as $80-$8B, one per key. They
		// exist for the guest to spend -- the window switch is the use
		// in view -- and the property this end guarantees is that they
		// are DISTINGUISHABLE: nothing else the cooker emits lands in
		// that range.
		kbd(0x00, 0x3A); released(); CHECK(typed() == 0x80);   // F1
		kbd(0x00, 0x3B); released(); CHECK(typed() == 0x81);   // F2
		kbd(0x00, 0x45); released(); CHECK(typed() == 0x8B);   // F12

		// Unmapped usages still produce nothing at all. Home, here --
		// this check used F1 until F1 acquired a meaning, which is the
		// same trap the reserved graphics sub-code fell into: a test
		// written about "nothing" quietly becomes a test about
		// something.
		kbd(0x00, 0x4A); released(); CHECK(typed() == -1);

		// A short keyboard report is ignored rather than read past.
		const uint8_t stub[4] = { 0, 0, 0x04, 0 };
		hid_cook_keyboard(stub, 4);
		CHECK(typed() == -1);

		// AUTO-REPEAT. A USB keyboard does not repeat, and the guest sees
		// no key-up, so the cooker repeats the held key on a clock the
		// test drives: 500 ms before the first repeat, then every 50 ms.
		{
			uint32_t t = 0xFFFFF000u;	// wraps mid-test, on purpose
			auto tick = [&](uint32_t ms) {
				t += ms;
				hid_cook_tick(t);
			};
			auto held = [&](uint8_t mod, uint8_t a, uint8_t b) {
				const uint8_t r[8] = { mod, 0, a, b, 0, 0, 0, 0 };
				hid_cook_keyboard(r, 8);
			};
			auto count = [&](int want) -> int {	// drain, tally `want`
				int n = 0;
				for (int c; (c = typed()) != -1;)
					n += c == want;
				return n;
			};

			hid_cook_reset();
			held(0, 0x04, 0);			// 'a' down
			tick(0);				// arms the delay
			CHECK(typed() == 'a');
			tick(499);
			CHECK(typed() == -1);			// not before the delay
			tick(1);
			CHECK(typed() == 'a');			// the first repeat
			int reps = 0;
			for (int i = 0; i < 10; i++) {
				tick(49);
				reps += typed() != -1;		// nothing early
				tick(1);
				reps += typed() == 'a' ? 100 : 0;
			}
			CHECK(reps == 1000);			// ten, each on time
			released();
			tick(1000);
			CHECK(typed() == -1);			// release stops it

			// A REPEAT WAITS FOR THE GUEST: nothing is queued behind a
			// byte the guest has not read, however long the key is
			// held -- so there is never a backlog to keep typing after
			// release. The moment the byte is taken, the repeat goes.
			held(0, 0x05, 0);			// 'b'
			tick(0);
			for (int i = 0; i < 50; i++)
				tick(50);			// 2.5 s, nothing read
			CHECK(count('b') == 1);			// the press alone
			tick(0);				// read, so the next goes
			CHECK(typed() == 'b');
			// ...and the one after it is a full interval later, not
			// a burst catching up on the repeats that were held back.
			tick(0);
			CHECK(typed() == -1);
			tick(49);
			CHECK(typed() == -1);
			tick(1);
			CHECK(typed() == 'b');

			// A new key takes the repeat over; the repeat produces
			// what the key produces NOW, so Shift mid-repeat is 'C'.
			held(0, 0x05, 0x06);			// 'c' down, 'b' held
			CHECK(typed() == 'c');
			tick(0);
			tick(500);
			CHECK(typed() == 'c');
			held(0x02, 0x05, 0x06);			// Shift, no new key
			tick(50);
			CHECK(typed() == 'C');
			released();
			tick(1000);
			CHECK(typed() == -1);

			// Function keys are the guest's, e.g. to switch
			// windows: they do not repeat.
			held(0, 0x3A, 0);			// F1
			CHECK(typed() == 0x80);
			tick(0);
			tick(2000);
			CHECK(typed() == -1);
			released();

			// Caps Lock does not repeat, and like any new key-down it
			// ends the repeat in progress.
			held(0, 0x04, 0);
			CHECK(typed() == 'a');
			held(0, 0x04, 0x39);			// + Caps Lock
			tick(0);
			tick(2000);
			CHECK(typed() == -1);
			released();
			held(0, 0x39, 0); released();		// caps off again
			CHECK(!hid_cook_caps_lock());

			// ErrorRollOver -- too many keys to say which -- is not a
			// report of what is held. It used to become the previous
			// state, so the next real report made every held key look
			// new and typed each of them again.
			held(0, 0x04, 0x05);
			CHECK(count('a') == 1);
			{
				const uint8_t r[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
				hid_cook_keyboard(r, 8);
			}
			held(0, 0x04, 0x05);			// same keys, still held
			CHECK(typed() == -1);
			released();
			hid_cook_reset();
		}

		// THE ECHO SWITCH ACTUALLY SWITCHES SOMETHING. The console's
		// verbose command drives this, and for months it drove nothing
		// at all -- a control the operator could set with no way to
		// tell "quiet" from "broken". Capture stdout and prove both
		// states.
		auto echo_output = [&](bool on) -> std::string {
			hid_cook_set_echo(on);
			fflush(stdout);
			FILE *tmp = tmpfile();
			const int saved = dup(1);
			dup2(fileno(tmp), 1);
			kbd(0x00, 0x04); released();    // 'a'
			// Release then press: the button echo reports EDGES, so
			// each pass must make one of its own -- the silent pass
			// would otherwise leave the button down and the loud one
			// would have nothing to report.
			const uint8_t up[]   = { 0x00, 0, 0 };
			const uint8_t down[] = { 0x01, 0, 0 };
			hid_cook_mouse(up, 3);
			hid_cook_mouse(down, 3);
			fflush(stdout);
			dup2(saved, 1);
			close(saved);
			rewind(tmp);
			std::string out;
			char c;
			while (fread(&c, 1, 1, tmp) == 1)
				out += c;
			fclose(tmp);
			return out;
		};

		const std::string quiet = echo_output(false);
		CHECK(quiet.empty());               // off means silent

		const std::string loud = echo_output(true);
		CHECK(loud.find("KEY") != std::string::npos);
		CHECK(loud.find("MOUSE") != std::string::npos);

		// WHAT THE KEY ECHO SAYS. Each line names the HID usage (the
		// USB "scancode"), the modifiers, and the byte the 6809 was
		// sent -- or why none was. Releases, modifier-only changes,
		// rollover reports and repeats are lines too: "I pressed it and
		// nothing happened" has several causes, and the echo is how an
		// operator tells them apart.
		{
			auto capture = [&](auto body) -> std::string {
				hid_cook_set_echo(true);
				fflush(stdout);
				FILE *tmp = tmpfile();
				const int saved = dup(1);
				dup2(fileno(tmp), 1);
				body();
				fflush(stdout);
				dup2(saved, 1);
				close(saved);
				rewind(tmp);
				std::string out;
				char c;
				while (fread(&c, 1, 1, tmp) == 1)
					out += c;
				fclose(tmp);
				hid_cook_set_echo(false);
				return out;
			};
			auto has = [](const std::string &out, const char *want) {
				const bool ok = out.find(want) != std::string::npos;
				if (!ok)
					fprintf(stderr, "  key echo lacks \"%s\" in:\n%s",
					    want, out.c_str());
				return ok;
			};
			hid_cook_reset();
			const std::string e = capture([&]() {
				kbd(0x02, 0x04); released();	// Shift-a
				kbd(0x01, 0x06); released();	// Ctrl-c
				kbd(0x00, 0x3A); released();	// F1
				kbd(0x00, 0x4A); released();	// Home: unmapped
				kbd(0x00, 0x39); released();	// Caps Lock
				kbd(0x00, 0x39); released();	// ...and off
				const uint8_t roll[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
				hid_cook_keyboard(roll, 8);
				kbd(0x00, 0x05);		// 'b', held to repeat
				while (typed() != -1)		// the guest reads, or
					;			// no repeat is queued
				hid_cook_tick(1000);
				hid_cook_tick(1500);
				released();
			});
			CHECK(has(e, "KEY down usage 0x04  mod 0x02 LShift"));
			CHECK(has(e, "-> 0x41 'A'"));
			CHECK(has(e, "KEY up   usage 0x04"));
			CHECK(has(e, "mod 0x01 LCtrl"));
			CHECK(has(e, "-> 0x03 ^C"));
			CHECK(has(e, "usage 0x3A"));
			CHECK(has(e, "-> 0x80 F1"));
			CHECK(has(e, "usage 0x4A"));
			CHECK(has(e, "-> nothing: no mapping for this key"));
			CHECK(has(e, "-> nothing: Caps Lock now ON"));
			CHECK(has(e, "-> nothing: Caps Lock now OFF"));
			CHECK(has(e, "KEY mods now 0x00 -"));
			CHECK(has(e, "rollover: too many keys"));
			CHECK(has(e, "KEY rpt  usage 0x05"));
			CHECK(has(e, "-> 0x62 'b'"));
			CHECK(!hid_cook_caps_lock());
			hid_cook_reset();
		}

		hid_cook_set_echo(false);           // leave the suite quiet
		while (typed() != -1)               // drain what the echo typed
			;

		v.write(STAT, VID_CTL_RESET);                   // back to normal
	}

	// ---- !IRQ: RIE-gated, follows RDRF, MC6850-shaped ----
	{
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		CHECK(picovideo_irq_released());        // reset leaves it idle
		CHECK(!(v.read(STAT) & VID_STAT_IRQ));

		// A polling guest must never be interrupted by a line it did
		// not ask for: input with RIE off changes nothing.
		vid_link_input('x');
		settle(v);
		CHECK(v.read(STAT) & VID_STAT_RDRF);    // byte is waiting
		CHECK(picovideo_irq_released());        // but the line is idle
		CHECK(!(v.read(STAT) & VID_STAT_IRQ));

		// Enabling RIE with a byte already waiting asserts immediately.
		v.write(STAT, VID_CTL_RIE);
		settle(v);
		CHECK(!picovideo_irq_released());
		CHECK(v.read(STAT) & VID_STAT_IRQ);     // bit 7 mirrors the pin

		// Reading the byte drains the FIFO, so RDRF clears and the line
		// releases -- the MC6850 behaviour the guest driver expects.
		CHECK(v.read(DATA) == 'x');
		settle(v);
		CHECK(!(v.read(STAT) & VID_STAT_RDRF));
		CHECK(picovideo_irq_released());
		CHECK(!(v.read(STAT) & VID_STAT_IRQ));

		// More input re-asserts while RIE stays on.
		vid_link_input('y');
		settle(v);
		CHECK(!picovideo_irq_released());

		// Reset drops the line: it flushes the very input that
		// justified asserting, and clears RIE with it.
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		CHECK(picovideo_irq_released());
		vid_link_input('z');
		settle(v);
		CHECK(picovideo_irq_released());        // RIE cleared by reset
		(void)v.read(DATA);
		v.write(STAT, VID_CTL_RESET);
		settle(v);
	}

	// ---- VID.Info ($FF13): the drawing surface, without a round trip ----
	{
		const Word INFO = 3;	// $FF13

		// Self-framing exactly as the mouse is: write to home, then
		// read the block. A guest that already talks to $FF12 needs no
		// new idiom.
		v.write(INFO, 0x00);
		Byte b[VID_INFO_LEN];
		for (int i = 0; i < (int)VID_INFO_LEN; i++)
			b[i] = v.read(INFO);

		// Pixels first, big-endian, so graphics code reads four bytes
		// and stops.
		CHECK(((b[0] << 8) | b[1]) == FB_WIDTH);
		CHECK(((b[2] << 8) | b[3]) == FB_HEIGHT);
		CHECK(b[4] == TEXT_COLS);
		CHECK(b[5] == TEXT_ROWS);

		// The index wraps, so an over-long read starts the block again
		// rather than walking into neighbouring registers.
		CHECK(v.read(INFO) == b[0]);

		// Homing needs no tick: on hardware the guest may read in the
		// instruction after the write, which would race the drain.
		(void)v.read(INFO);
		(void)v.read(INFO);
		v.write(INFO, 0xFF);
		CHECK(v.read(INFO) == b[0]);

		// Read-only: a write homes the index, it does not store.
		CHECK(v.read(INFO) != 0xFF);

		// And the scratch registers above it still behave as scratch,
		// which is what proves the bus path from the console.
		v.write(4, 0x5A);
		settle(v);
		CHECK(v.read(4) == 0x5A);
	}

	// M5.1: a window is a text screen that swaps into the ONE live layer.
	// The properties worth pinning are that text is CONFINED to it, that
	// switching PRESERVES what each window held, and that VID.Info reports
	// the window actually built rather than the panel.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);		// clear, root window
		settle(v);

		// A window inland: id 1, at column 4 row 2, 10 x 3.
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t dw[8] = { 1, 0, 4, 2, 10, 3, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, dw[i]);
		// Defining does not display: a guest setting up several
		// windows should not watch them flicker past. Show it.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		v.write(DATA, 'A');
		settle(v);

		// Text lands INSIDE the window, at its origin -- not at 0,0.
		CHECK(check_cell(4, 2, 'A'));
		CHECK(!check_cell(0, 0, 'A'));

		// VID.Info now describes the WINDOW: 10x3 cells, 80x48 pixels.
		v.write(3, 0);			// home the index
		CHECK(v.read(3) == 0);		// width hi  (80 = 0x0050)
		CHECK(v.read(3) == 80);		// width lo
		CHECK(v.read(3) == 0);		// height hi (48)
		CHECK(v.read(3) == 48);		// height lo
		CHECK(v.read(3) == 10);		// cols
		CHECK(v.read(3) == 3);		// rows

		// Wrap happens at the WINDOW edge, not the panel edge: nine
		// more characters fill row 0 and the tenth starts row 1.
		for (int i = 0; i < 10; i++) v.write(DATA, 'B');
		settle(v);
		CHECK(check_cell(13, 2, 'B'));	// last column of the window
		CHECK(check_cell(4, 3, 'B'));	// wrapped to the window's row 1
		CHECK(!check_cell(14, 2, 'B'));	// never outside it

		// Select does NOT change what is displayed -- that is DWShow's
		// job. A descriptor may Select before every write burst, so a
		// visible effect here would flicker the screen once per line
		// of background output.
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		settle(v);
		CHECK(check_cell(4, 2, 'A'));	// still on screen

		// Showing another window is what swaps the display.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		settle(v);
		CHECK(!check_cell(4, 2, 'A'));	// window 1 is put away
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 1);
		settle(v);
		CHECK(check_cell(4, 2, 'A'));	// and comes back intact
		CHECK(check_cell(13, 2, 'B'));	// to the end of its first row
		CHECK(check_cell(4, 3, 'B'));	// AND on the rows below it --
						// parking only the first row
						// passes every check above

		// Selecting the root reports the panel again.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		settle(v);
		v.write(3, 0);		// home AFTER the select is consumed
		v.read(3); v.read(3); v.read(3); v.read(3);
		CHECK(v.read(3) == TEXT_COLS);
		CHECK(v.read(3) == TEXT_ROWS);

		// Clamping: a window asked for beyond the panel is built
		// smaller and SAYS SO, rather than being refused or silently
		// overrunning. Asking for 20 columns at column 120 can only
		// have 8.
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t dw2[8] = { 2, 0, 120, 0, 20, 2, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, dw2[i]);
		settle(v);
		v.write(3, 0);
		v.read(3); v.read(3); v.read(3); v.read(3);
		CHECK(v.read(3) == 8);		// clamped, and reported
		CHECK(v.read(3) == 2);

		// A window that is NOT displayed still takes output: it
		// accumulates in its parked screen and appears when shown.
		// This is the whole point of splitting Select from DWShow --
		// a background shell must be able to write without stealing
		// the display.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 1);
		v.write(DATA, 0x0C);		// clear window 1, off screen
		v.write(DATA, 'Q');
		settle(v);
		CHECK(!check_cell(4, 2, 'Q'));	// nothing appeared on screen
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		settle(v);
		CHECK(check_cell(4, 2, 'Q'));	// and it was there all along

		// Leave the console on the root window: everything after this
		// draws full-screen and would otherwise inherit a rectangle.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		v.write(DATA, 0x0C);
		settle(v);
	}

	// M5.2: graphics primitives are window-RELATIVE and window-BOUNDED.
	// There is still only one graphics plane -- clipping says where a
	// primitive lands, not which surface it lands on -- so both halves
	// need asserting: that 0,0 means the window's corner, and that pixels
	// cannot escape its edges.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// Window at cell 4,2 = pixel 32,32; 10x3 cells = 80x48 pixels.
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t g[8] = { 1, 0, 4, 2, 10, 3, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, g[i]);
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		settle(v);

		// Plot at window 20,20 -- it must land 20 pixels in from the
		// WINDOW's corner (32,32), so at 52,52, not at the screen's.
		// Deliberately not 0,0: that cell holds the text cursor, whose
		// save-under and inversion would decide the pixel instead of
		// the plot, and the point here is the coordinate origin.
		v.write(DATA, ESC); v.write(DATA, 0x42);
		v.write(DATA, 0); v.write(DATA, 20);	// x = 20
		v.write(DATA, 0); v.write(DATA, 20);	// y = 20
		settle(v);
		CHECK(video_framebuffer[52][52] != 0x00);
		CHECK(video_framebuffer[20][20] == 0x00);  // not screen-origin

		// A bar far larger than the window must be cut off at its
		// edges rather than painting the panel. Draw pointer to the
		// window's corner first, then Bar ($4A, absolute) to 200,200 --
		// four times the window's width and height.
		v.write(DATA, ESC); v.write(DATA, 0x40);	// SetDPtr
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x4A);	// Bar
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		settle(v);
		CHECK(video_framebuffer[40][100] != 0x00);  // inside (32..111)
		CHECK(video_framebuffer[40][112] == 0x00);  // past the right
		CHECK(video_framebuffer[80][40]  == 0x00);  // past the bottom

		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		v.write(DATA, 0x0C);
		settle(v);
	}

	// DWProtSw with an OUTLINE rather than a fill. Box and Bar reach the
	// framebuffer by different routes -- hline/vline against fill_rect --
	// so a clip that opens for one can stay shut for the other. These are
	// the exact bytes a guest demo sent when it reported the escape not
	// working, kept because a second path is what the Bar case misses.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t w2[8] = { 2, 0, 16, 5, 48, 16, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, w2[i]);
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 2);
		settle(v);
		// 1B 36 00 / 1B 32 01 / 1B 40 00 00 00 00 / 1B 48 02 58 01 F4
		v.write(DATA, ESC); v.write(DATA, 0x36); v.write(DATA, 0x00);
		v.write(DATA, ESC); v.write(DATA, 0x32); v.write(DATA, 0x01);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0x00); v.write(DATA, 0x00);
		v.write(DATA, 0x00); v.write(DATA, 0x00);
		v.write(DATA, ESC); v.write(DATA, 0x48);
		v.write(DATA, 0x02); v.write(DATA, 0x58);
		v.write(DATA, 0x01); v.write(DATA, 0xF4);
		settle(v);
		// Window rect is px (128,80)-(512,336). The box's top edge
		// runs to x = 128+600 = 728, well past the window's right
		// edge at 512, so unprotected it must appear out there.
		CHECK(video_framebuffer[80][600] != 0x00);  // outside, drawn
		CHECK(video_framebuffer[80][400] != 0x00);  // and inside too
	}

	// M5.3: CWArea narrows the working area within a window; DWProtSw
	// decides whether graphics may leave it.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// Window 1 at cell 4,2, 10x3.
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t w1[8] = { 1, 0, 4, 2, 10, 3, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, w1[i]);
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		settle(v);

		// Narrow the working area to 4x2 at offset 2,1 inside it, so
		// the effective rectangle is cell 6,3 and the read-back must
		// say 4x2 rather than the window's 10x3.
		v.write(DATA, ESC); v.write(DATA, 0x25);
		v.write(DATA, 2); v.write(DATA, 1);
		v.write(DATA, 4); v.write(DATA, 2);
		settle(v);
		v.write(3, 0);
		v.read(3); v.read(3); v.read(3); v.read(3);
		CHECK(v.read(3) == 4);		// cols of the WORKING AREA
		CHECK(v.read(3) == 2);

		// Text now starts at the working area's corner, not the
		// window's.
		v.write(DATA, 'W');
		settle(v);
		CHECK(check_cell(6, 3, 'W'));
		CHECK(!check_cell(4, 2, 'W'));

		// CWArea is clamped to the window just as DWSet is clamped to
		// the panel: asking for 40 columns in a 10-wide window can
		// only get 10, and the read-back says so.
		v.write(DATA, ESC); v.write(DATA, 0x25);
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, 40); v.write(DATA, 40);
		settle(v);
		v.write(3, 0);
		v.read(3); v.read(3); v.read(3); v.read(3);
		CHECK(v.read(3) == 10);
		CHECK(v.read(3) == 3);

		// Protected first, because the graphics plane is global and
		// accumulates: once something has painted outside the window
		// nothing can un-paint it, so "contained" has to be checked
		// before anything is allowed to escape.
		v.write(DATA, ESC); v.write(DATA, 0x40);	// SetDPtr 0,0
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x4A);	// Bar 200x200
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		settle(v);
		CHECK(video_framebuffer[80][120] == 0x00);  // contained

		// Protection off lets it out; coordinates still measure from
		// the window's corner, so nothing appears above or left of it.
		v.write(DATA, ESC); v.write(DATA, 0x36); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		settle(v);
		CHECK(video_framebuffer[80][120] != 0x00);  // outside, drawn
		CHECK(video_framebuffer[20][20] == 0x00);   // still not above

		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		v.write(DATA, 0x0C);
		settle(v);
	}

	// A blank cell in a row where NO graphics has been drawn, while other
	// rows HAVE been. That mixture is what every real screen looks like
	// and what no previous test here produced: they either clear and draw
	// nothing, so no row is materialised and the safe branch runs, or they
	// draw over the cells they check, so the row is materialised.
	//
	// In between, the blitter dereferenced a null row pointer and read
	// from address x -- the bootrom -- painting unrelated bytes into the
	// framebuffer wherever text touched an undrawn region.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// Draw a bar well down the screen, so rows near the TOP are
		// left unmaterialised while rows lower down are real.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, 1); v.write(DATA, 0x2C);	// y 300
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 2); v.write(DATA, 0);	// to 512,400
		v.write(DATA, 1); v.write(DATA, 0x90);
		settle(v);

		// Now write text near the top and erase it. The erase restores
		// the graphics under it -- from a row nothing has drawn on.
		v.write(DATA, 0x02); v.write(DATA, 0x28); v.write(DATA, 0x22);
		v.write(DATA, 'X');
		settle(v);
		v.write(DATA, 0x02); v.write(DATA, 0x28); v.write(DATA, 0x22);
		v.write(DATA, ' ');
		settle(v);

		// Cell 8,2 is pixels 64..71 x 32..47: nothing was ever drawn
		// there, so every pixel must be the background the screen was
		// cleared to -- not bootrom, and not anything else.
		vidterm_cursor_hide();
		for (int gy = 0; gy < CHAR_H; gy++)
			for (int gx = 0; gx < CHAR_W; gx++)
				CHECK(video_framebuffer[32 + gy][64 + gx] == 0x00);
	}

	// Pattern tiling checked against the graphics demo's REAL 16x16
	// weave, at a fill origin that is not a multiple of 16.
	//
	// Both halves matter. A 16-wide pattern does not divide evenly into
	// an arbitrary coordinate, and an unaligned origin is the only way to
	// tell tiling in SCREEN coordinates from tiling restarted at each
	// fill's own corner -- which is what a pattern must not do, or
	// abutting fills would not line up. The suite's other pattern test
	// uses a 4x4 at an aligned origin and would pass either way.
	{
		static const uint8_t ESC = 0x1B;
		static const uint8_t weave[256] = {
			0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43,
			0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0,
			0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0,
			0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8,
			0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8,
			0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13,
			0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13,
			0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43,
			0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43,
			0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0,
			0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0,
			0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8,
			0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8,
			0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13,
			0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13,
			0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43, 0x43, 0xE0, 0xE0, 0xF8, 0xF8, 0x13, 0x13, 0x43,
		};

		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 3); v.write(DATA, 1);
		v.write(DATA, 1); v.write(DATA, 0);		// 256 bytes
		v.write(DATA, ESC); v.write(DATA, 0x2B);
		v.write(DATA, 3); v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 16);
		v.write(DATA, 0); v.write(DATA, 16);
		v.write(DATA, 1); v.write(DATA, 0);
		for (int i = 0; i < 256; i++) {
			v.write(DATA, weave[i]);
			if ((i & 0x7F) == 0x7F)
				settle(v);
		}
		for (int t = 0; t < 8; t++)
			settle(v);

		v.write(DATA, ESC); v.write(DATA, 0x2E);
		v.write(DATA, 3); v.write(DATA, 1);
		// Fill from 101,205. The origin is chosen AGAINST THE
		// PATTERN'S SYMMETRY, not merely off a tile boundary. This
		// weave is diagonal, so it is unchanged by any shift whose
		// dx+dy is a multiple of 8 -- and at 101,203, which is
		// unaligned and looks perfectly reasonable, tiling from the
		// fill's own corner produces a pixel-for-pixel IDENTICAL
		// result to tiling in screen coordinates. The test would have
		// been blind to the one bug it exists to catch. At 101,205
		// every pixel of the block differs.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 101);
		v.write(DATA, 0); v.write(DATA, 205);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 180);
		v.write(DATA, 1); v.write(DATA, 0x0E);		// 270
		settle(v);

		/* Dense over two full tiles, not sampled. The weave is
		 * diagonal and therefore self-similar under a shift, so a
		 * strided scan agrees with a mis-aligned lookup at most of
		 * the points it happens to visit -- checking every pixel of a
		 * 32x32 block is what makes a wrong origin unmissable rather
		 * than merely unlikely. */
		for (int y = 205; y < 237; y++)
			for (int x = 101; x < 133; x++)
				CHECK(video_framebuffer[y][x] ==
				    weave[(y % 16) * 16 + (x % 16)]);

		/* Plus the far corner, to prove it is still in phase after
		 * several tiles rather than only near the origin. */
		CHECK(video_framebuffer[269][179] ==
		    weave[(269 % 16) * 16 + (179 % 16)]);

		// THE PATTERN RULE IS CoCo3's (GrfDrv, level2/cmds/grfdrv.asm;
		// VID-033): EVERY pixel-plotting primitive takes the pattern,
		// PutBlk included, and the pixel written is colour AND pattern.
		// An earlier version of this test asserted the opposite --
		// outlines solid, stamps untouched -- because the firmware had
		// confined the pattern to fills. That hid the real fault, which
		// was a stamp REPLACED by the pattern rather than ANDed with it.
		v.write(DATA, ESC); v.write(DATA, 0x2E);
		v.write(DATA, 3); v.write(DATA, 1);	// pattern on

		// An OUTLINE is woven: FColor 7 is white, so its pixels are the
		// pattern's own, tiled from the window origin (the root, 0,0).
		// Box from 600,300 to 700,360; check its whole top edge.
		v.write(DATA, ESC); v.write(DATA, 0x32); v.write(DATA, 7);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 2); v.write(DATA, 0x58);	// 600,300
		v.write(DATA, 1); v.write(DATA, 0x2C);
		v.write(DATA, ESC); v.write(DATA, 0x48);	// Box
		v.write(DATA, 2); v.write(DATA, 0xBC);	// to 700,360
		v.write(DATA, 1); v.write(DATA, 0x68);
		settle(v);
		for (int x = 600; x <= 700; x++)
			CHECK(video_framebuffer[300][x] ==
			    (uint8_t)(0xFF & weave[(300 % 16) * 16 + (x % 16)]));

		// A STAMP is the image ANDed with the pattern -- NOT the pattern
		// in place of the image (the original fault) and NOT the bare
		// image (the over-correction). g3/b1 is the weave itself,
		// stamped at 601,451, where its own corner is out of phase with
		// the screen tiling, so all three readings differ.
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 3); v.write(DATA, 1);
		v.write(DATA, 2); v.write(DATA, 0x59);	// x 601
		v.write(DATA, 1); v.write(DATA, 0xC3);	// y 451
		settle(v);
		for (int r = 0; r < 16; r++)
			for (int c = 0; c < 16; c++)
				CHECK(video_framebuffer[451 + r][601 + c] ==
				    (uint8_t)(weave[r * 16 + c] &
				    weave[((451 + r) % 16) * 16 +
				    ((601 + c) % 16)]));

		// And FFill, which IS a fill, must take the pattern. Box an
		// enclosure in solid, then flood inside it.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 1); v.write(DATA, 0x90);	// 200,400
		v.write(DATA, ESC); v.write(DATA, 0x48);
		v.write(DATA, 1); v.write(DATA, 0x18);	// to 280,460
		v.write(DATA, 1); v.write(DATA, 0xCC);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 240);
		v.write(DATA, 1); v.write(DATA, 0xAE);	// 240,430 inside it
		v.write(DATA, ESC); v.write(DATA, 0x4F);	// FFill
		settle(v);
		CHECK(video_framebuffer[430][240] ==
		    weave[(430 % 16) * 16 + (240 % 16)]);
		CHECK(video_framebuffer[431][241] ==
		    weave[(431 % 16) * 16 + (241 % 16)]);

		// Killing the buffer a pattern came from must drop the
		// pattern. It is a kept pointer now -- into an SRAM cache
		// when it fitted, into the pool when it did not -- so a kill
		// that left it selected would be drawing from storage the
		// allocator is free to hand out again.
		v.write(DATA, ESC); v.write(DATA, 0x2A);
		v.write(DATA, 3); v.write(DATA, 1);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 1); v.write(DATA, 0x2C);	// 300,300
		v.write(DATA, 1); v.write(DATA, 0x2C);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 1); v.write(DATA, 0x5E);	// to 350,350
		v.write(DATA, 1); v.write(DATA, 0x5E);
		settle(v);
		// Solid again: every pixel of the new fill the same.
		for (int y = 301; y < 349; y += 7)
			for (int x = 301; x < 349; x += 7)
				CHECK(video_framebuffer[y][x] ==
				    video_framebuffer[300][300]);

		v.write(DATA, ESC); v.write(DATA, 0x2E);
		v.write(DATA, 3); v.write(DATA, 0);
	}

	// The graphics demo's exact scene-3 byte sequence, reproduced because
	// it was reported as drawing nothing. A 32x32 image, loaded and
	// stamped, at the sizes and in the order the guest sends them.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// 1B 29 01 01 04 00
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 0x01); v.write(DATA, 0x01);
		v.write(DATA, 0x04); v.write(DATA, 0x00);
		// 1B 2B 01 01 01 00 20 00 20 04 00 + 1024 bytes
		v.write(DATA, ESC); v.write(DATA, 0x2B);
		v.write(DATA, 0x01); v.write(DATA, 0x01); v.write(DATA, 0x01);
		v.write(DATA, 0x00); v.write(DATA, 0x20);
		v.write(DATA, 0x00); v.write(DATA, 0x20);
		v.write(DATA, 0x04); v.write(DATA, 0x00);
		for (int i = 0; i < 1024; i++) {
			v.write(DATA, (uint8_t)(0x20 + (i & 0x3F)));
			if ((i & 0x7F) == 0x7F)
				settle(v);
		}
		for (int t = 0; t < 16; t++)
			settle(v);

		// 1B 2D 01 01 <X16> <Y16> at 64,64
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 0x01); v.write(DATA, 0x01);
		v.write(DATA, 0x00); v.write(DATA, 0x40);
		v.write(DATA, 0x00); v.write(DATA, 0x40);
		settle(v);

		CHECK(video_framebuffer[64][64] == 0x20);	// first pixel
		CHECK(video_framebuffer[64][95] == 0x3F);	// end of its row
		CHECK(video_framebuffer[95][64] != 0x00);	// 32 rows deep
		CHECK(video_framebuffer[64][96] == 0x00);	// and no wider

		// A GetBlk whose rectangle runs off the panel must not read
		// past the plane. Before this was bounded it indexed the
		// row-valid map past its end and read past a shadow row --
		// invisible on a host, where one array simply runs into the
		// next, and a candidate bus fault on hardware where PSRAM
		// beyond the declared region is configured to fault. Off-panel
		// pixels come back as background.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 0x01); v.write(DATA, 0x09);
		v.write(DATA, 0x04); v.write(DATA, 0x00);
		v.write(DATA, ESC); v.write(DATA, 0x2C);
		v.write(DATA, 0x01); v.write(DATA, 0x09);
		v.write(DATA, 0x03); v.write(DATA, 0xF0);	// x 1008
		v.write(DATA, 0x01); v.write(DATA, 0xD0);	// y 464
		v.write(DATA, 0x00); v.write(DATA, 0x20);	// w 32 -> 1040
		v.write(DATA, 0x00); v.write(DATA, 0x20);	// h 32 -> 496
		settle(v);
		// Stamping it back must not crash and must be background in
		// the part that came from off-panel.
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 0x01); v.write(DATA, 0x09);
		v.write(DATA, 0x01); v.write(DATA, 0x90);	// x 400
		v.write(DATA, 0x00); v.write(DATA, 0x64);	// y 100
		settle(v);
		CHECK(video_framebuffer[100 + 31][400 + 31] == 0x00);

		// GetBlk that region into g1/b2 and stamp it elsewhere.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 0x01); v.write(DATA, 0x02);
		v.write(DATA, 0x04); v.write(DATA, 0x00);
		v.write(DATA, ESC); v.write(DATA, 0x2C);
		v.write(DATA, 0x01); v.write(DATA, 0x02);
		v.write(DATA, 0x00); v.write(DATA, 0x40);
		v.write(DATA, 0x00); v.write(DATA, 0x40);
		v.write(DATA, 0x00); v.write(DATA, 0x20);
		v.write(DATA, 0x00); v.write(DATA, 0x20);
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 0x01); v.write(DATA, 0x02);
		v.write(DATA, 0x01); v.write(DATA, 0x00);	// x 256
		v.write(DATA, 0x00); v.write(DATA, 0x40);
		settle(v);
		CHECK(video_framebuffer[64][256] == 0x20);
		CHECK(video_framebuffer[64][287] == 0x3F);
	}

	// Text face, bold, and the cursor sprite. A loaded face must actually
	// replace the built-in one, and selecting buffer 0 must put it back --
	// the second half is what a one-way switch would fail.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// A face where every glyph is a solid block: 256 x 16 bytes
		// of 0xFF. Whatever character is printed, the cell fills.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 2); v.write(DATA, 1);
		v.write(DATA, 0x10); v.write(DATA, 0x00);	// 4096 bytes
		v.write(DATA, ESC); v.write(DATA, 0x2B);
		v.write(DATA, 2); v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 8);
		v.write(DATA, 0); v.write(DATA, 16);
		v.write(DATA, 0x10); v.write(DATA, 0x00);	// bufSize 4096
		// Drained as it goes: the command FIFO is 2 KB and the render
		// drain takes at most 256 bytes a pass, so a 4 KB payload
		// pushed in one breath would overflow and desynchronise the
		// stream. A real guest is flow-controlled by TDRE; this test
		// has to do it by hand.
		for (int i = 0; i < 4096; i++) {
			v.write(DATA, 0xFF);
			if ((i & 0x7F) == 0x7F)
				settle(v);
		}
		for (int t = 0; t < 32; t++)
			settle(v);

		// Before selecting it, 'A' renders as 'A'.
		v.write(DATA, 0x02); v.write(DATA, 0x20); v.write(DATA, 0x20);
		v.write(DATA, 'A');
		settle(v);
		CHECK(check_cell(0, 0, 'A'));

		// Select the face: now every glyph is solid, so the cell is
		// full where the real 'A' has gaps.
		v.write(DATA, ESC); v.write(DATA, 0x3A);
		v.write(DATA, 2); v.write(DATA, 1);
		v.write(DATA, 0x02); v.write(DATA, 0x21); v.write(DATA, 0x20);
		v.write(DATA, 'A');
		settle(v);
		vidterm_cursor_hide();
		for (int gy = 0; gy < CHAR_H; gy++)
			for (int gx = 0; gx < CHAR_W; gx++)
				CHECK(video_framebuffer[gy][8 + gx] == 0xFF);

		// Buffer 0 puts the built-in face back.
		v.write(DATA, ESC); v.write(DATA, 0x3A);
		v.write(DATA, 2); v.write(DATA, 0);
		v.write(DATA, 0x02); v.write(DATA, 0x22); v.write(DATA, 0x20);
		v.write(DATA, 'A');
		settle(v);
		CHECK(check_cell(2, 0, 'A'));

		// Killing the buffer a face came from must drop the
		// selection, not leave a pointer into storage the pool can
		// hand out again.
		v.write(DATA, ESC); v.write(DATA, 0x3A);
		v.write(DATA, 2); v.write(DATA, 1);
		v.write(DATA, ESC); v.write(DATA, 0x2A);
		v.write(DATA, 2); v.write(DATA, 1);
		v.write(DATA, 0x02); v.write(DATA, 0x23); v.write(DATA, 0x20);
		v.write(DATA, 'A');
		settle(v);
		CHECK(check_cell(3, 0, 'A'));	// built-in face, not garbage

		// Bold thickens a stroke: the cell gains lit pixels and keeps
		// every one it had.
		v.write(DATA, 0x0C);
		v.write(DATA, 0x02); v.write(DATA, 0x20); v.write(DATA, 0x20);
		v.write(DATA, 'H');
		settle(v);
		vidterm_cursor_hide();
		int plain_lit = 0;
		for (int gy = 0; gy < CHAR_H; gy++)
			for (int gx = 0; gx < CHAR_W; gx++)
				if (video_framebuffer[gy][gx] == 0xFF)
					plain_lit++;

		v.write(DATA, ESC); v.write(DATA, 0x3D); v.write(DATA, 1);
		v.write(DATA, 0x02); v.write(DATA, 0x21); v.write(DATA, 0x20);
		v.write(DATA, 'H');
		settle(v);
		vidterm_cursor_hide();
		int bold_lit = 0;
		for (int gy = 0; gy < CHAR_H; gy++)
			for (int gx = 0; gx < CHAR_W; gx++)
				if (video_framebuffer[gy][8 + gx] == 0xFF)
					bold_lit++;
		CHECK(bold_lit > plain_lit);
		v.write(DATA, ESC); v.write(DATA, 0x3D); v.write(DATA, 0);
	}

	// Get/put buffers: define, load, stamp, capture, and use as a fill
	// pattern. The round trip is the test that matters -- a block taken
	// off the screen and put back somewhere else must arrive intact.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// DefGPB group 1 buffer 1, 16 bytes: a 4x4 bitmap.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 16);
		// GPLoad it: type 1, 4x4, 16 bytes of payload.
		v.write(DATA, ESC); v.write(DATA, 0x2B);
		v.write(DATA, 1); v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 4);	// Xsize
		v.write(DATA, 0); v.write(DATA, 4);	// Ysize
		v.write(DATA, 0); v.write(DATA, 16);	// bufSize
		for (uint8_t i = 0; i < 16; i++)
			v.write(DATA, (uint8_t)(0x41 + i));
		v.write(DATA, 'Z');	// the stream must still be in step
		settle(v);
		CHECK(check_cell(0, 0, 'Z'));

		// PutBlk it at 500,300 and the pixels must be what was loaded.
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 1); v.write(DATA, 0xF4);	// x = 500
		v.write(DATA, 1); v.write(DATA, 0x2C);	// y = 300
		settle(v);
		CHECK(video_framebuffer[300][500] == 0x41);
		CHECK(video_framebuffer[300][503] == 0x44);
		CHECK(video_framebuffer[303][503] == 0x50);
		CHECK(video_framebuffer[300][504] != 0x45);   // 4 wide, not 5

		// GetBlk that same region into a second buffer, then stamp it
		// elsewhere: a round trip through our storage.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 1); v.write(DATA, 2);
		v.write(DATA, 0); v.write(DATA, 16);
		v.write(DATA, ESC); v.write(DATA, 0x2C);
		v.write(DATA, 1); v.write(DATA, 2);
		v.write(DATA, 1); v.write(DATA, 0xF4);	// x 500
		v.write(DATA, 1); v.write(DATA, 0x2C);	// y 300
		v.write(DATA, 0); v.write(DATA, 4);	// w 4
		v.write(DATA, 0); v.write(DATA, 4);	// h 4
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 1); v.write(DATA, 2);
		v.write(DATA, 2); v.write(DATA, 0x58);	// x 600
		v.write(DATA, 1); v.write(DATA, 0x2C);	// y 300
		settle(v);
		CHECK(video_framebuffer[300][600] == 0x41);
		CHECK(video_framebuffer[303][603] == 0x50);

		// PSet: fills take their colour from the pattern, tiled in
		// SCREEN coordinates so a fill does not restart it at its own
		// corner.
		v.write(DATA, ESC); v.write(DATA, 0x2E);
		v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, 1); v.write(DATA, 0x90);	// y = 400
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 140);
		v.write(DATA, 1); v.write(DATA, 0xB8);	// y = 440
		settle(v);
		// 4x4 pattern tiled from the screen origin: (100,400) is a
		// multiple of 4 in both axes, so it takes the pattern's 0,0.
		CHECK(video_framebuffer[400][100] == 0x41);
		CHECK(video_framebuffer[400][101] == 0x42);
		CHECK(video_framebuffer[401][100] == 0x45);
		CHECK(video_framebuffer[400][104] == 0x41);   // tiles

		// Buffer 0 turns it off again and fills go solid.
		v.write(DATA, ESC); v.write(DATA, 0x2E);
		v.write(DATA, 1); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, 1); v.write(DATA, 0x90);	// y = 400
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 140);
		v.write(DATA, 1); v.write(DATA, 0xB8);	// y = 440
		settle(v);
		CHECK(video_framebuffer[400][100] == video_framebuffer[400][101]);

		// A GPLoad with a reserved screenType must NOT load -- that is
		// what reserving zero is for, so an all-zero or truncated
		// header is rejected rather than rendered as whatever it
		// happens to contain. The payload is still counted and
		// dropped, so the stream stays in step: the 'Y' after it must
		// land as a character, not as pixels.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 1); v.write(DATA, 3);
		v.write(DATA, 0); v.write(DATA, 16);
		v.write(DATA, ESC); v.write(DATA, 0x2B);
		v.write(DATA, 1); v.write(DATA, 3); v.write(DATA, 0);  // type 0
		v.write(DATA, 0); v.write(DATA, 4);
		v.write(DATA, 0); v.write(DATA, 4);
		v.write(DATA, 0); v.write(DATA, 16);
		for (uint8_t i = 0; i < 16; i++)
			v.write(DATA, 0x77);
		v.write(DATA, 0x0C);			// clear, then a glyph
		v.write(DATA, 'Y');
		settle(v);
		CHECK(check_cell(0, 0, 'Y'));		// stream stayed in step

		const uint8_t untouched = video_framebuffer[350][800];
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 1); v.write(DATA, 3);
		v.write(DATA, 3); v.write(DATA, 0x20);	// x 800
		v.write(DATA, 1); v.write(DATA, 0x5E);	// y 350
		settle(v);
		CHECK(video_framebuffer[350][800] == untouched);  // nothing loaded

		// KillBuf then use it: accepted, does nothing, draws nothing.
		v.write(DATA, ESC); v.write(DATA, 0x2A);
		v.write(DATA, 1); v.write(DATA, 1);
		const uint8_t before = video_framebuffer[300][700];
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 2); v.write(DATA, 0xBC);	// x 700
		v.write(DATA, 1); v.write(DATA, 0x2C);
		settle(v);
		CHECK(video_framebuffer[300][700] == before);
	}

	// LSet: the raster mix mode. XOR is the one that proves it, because
	// drawing the same thing twice must return the screen to where it
	// started -- which no other mode does and no accident reproduces.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// A bar to combine against, then note what is under it.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		settle(v);
		const uint8_t under = video_framebuffer[150][150];
		CHECK(under != 0x00);

		// XOR a second bar across it, twice. The first changes the
		// pixels; the second must put them back exactly.
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 3);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 180);
		v.write(DATA, 0); v.write(DATA, 180);
		settle(v);
		const uint8_t xored = video_framebuffer[150][150];
		CHECK(xored != under);		// something happened

		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 180);
		v.write(DATA, 0); v.write(DATA, 180);
		settle(v);
		CHECK(video_framebuffer[150][150] == under);   // and undone

		// Back to replace, and a bar really does replace again.
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x32); v.write(DATA, 2);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 180);
		v.write(DATA, 0); v.write(DATA, 180);
		settle(v);
		const uint8_t replaced = video_framebuffer[150][150];
		CHECK(replaced != under);
		CHECK(replaced != xored);	// not the XOR result either

		// WHAT THE LOGIC COMBINES AGAINST. A mix mode must read the
		// GRAPHICS plane, not the framebuffer -- the framebuffer may
		// have text on it, and a guest XORing across a window full of
		// characters means to combine with the drawing underneath
		// them, not with the glyphs.
		//
		// Reading the framebuffer instead is invisible until text is
		// present, which is why this check puts a character in the
		// way: XOR it twice (a no-op), then erase the character. The
		// graphics underneath must be exactly what it was. Combining
		// against the framebuffer feeds glyph pixels into the shadow,
		// and erasing then paints the ghost of the character back as
		// though it were drawing.
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x32); v.write(DATA, 3);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		settle(v);
		const uint8_t plain = video_framebuffer[176][152];

		v.write(DATA, 0x02);			// cursor to cell 19,11
		v.write(DATA, (uint8_t)(19 + 0x20));
		v.write(DATA, (uint8_t)(11 + 0x20));
		v.write(DATA, 'M');			// a dense glyph
		settle(v);

		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 3);
		for (int pass = 0; pass < 2; pass++) {
			v.write(DATA, ESC); v.write(DATA, 0x40);
			v.write(DATA, 0); v.write(DATA, 150);
			v.write(DATA, 0); v.write(DATA, 170);
			v.write(DATA, ESC); v.write(DATA, 0x4A);
			v.write(DATA, 0); v.write(DATA, 170);
			v.write(DATA, 0); v.write(DATA, 190);
			settle(v);
		}
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 0);

		v.write(DATA, 0x02);			// erase the character
		v.write(DATA, (uint8_t)(19 + 0x20));
		v.write(DATA, (uint8_t)(11 + 0x20));
		v.write(DATA, ' ');
		settle(v);
		for (int gy = 0; gy < CHAR_H; gy++)
			for (int gx = 0; gx < CHAR_W; gx++)
				CHECK(video_framebuffer[176 + gy][152 + gx] == plain);

		// A mode the guest should not send falls back to replace
		// rather than doing something inventive.
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 99);
		v.write(DATA, ESC); v.write(DATA, 0x32); v.write(DATA, 1);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 180);
		v.write(DATA, 0); v.write(DATA, 180);
		settle(v);
		CHECK(video_framebuffer[150][150] != replaced);  // it drew
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 0);
	}

	// Flood fill. What a wrong one does is leak past its boundary, or
	// fail to reach a corner, or never stop -- so the shape under test is
	// a hollow box, and the checks are inside it, on its wall, and
	// outside it.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// A hollow box from 100,100 to 300,250, drawn in the
		// foreground colour.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, ESC); v.write(DATA, 0x48);	// Box
		v.write(DATA, 1); v.write(DATA, 0x2C);		// 300,250
		v.write(DATA, 0); v.write(DATA, 250);
		settle(v);
		const uint8_t wall = video_framebuffer[100][200];
		CHECK(wall != 0x00);
		CHECK(video_framebuffer[175][200] == 0x00);	// hollow inside

		// Seed the fill in the middle of it.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 175);
		v.write(DATA, ESC); v.write(DATA, 0x4F);	// FFill
		settle(v);

		CHECK(video_framebuffer[175][200] != 0x00);	// centre filled
		CHECK(video_framebuffer[101][101] != 0x00);	// and the corners
		CHECK(video_framebuffer[249][299] != 0x00);
		// It must NOT get out. One pixel beyond each wall is the test
		// that matters: a fill that leaks takes the whole screen.
		CHECK(video_framebuffer[175][99]  == 0x00);
		CHECK(video_framebuffer[175][301] == 0x00);
		CHECK(video_framebuffer[99][200]  == 0x00);
		CHECK(video_framebuffer[251][200] == 0x00);

		// Filling with the colour already there must do nothing at
		// all, rather than seeding itself forever -- every pixel it
		// examined would match after being written.
		v.write(DATA, ESC); v.write(DATA, 0x4F);
		settle(v);
		CHECK(video_framebuffer[175][200] != 0x00);	// still here

		// And no seeds were dropped: this shape is nowhere near the
		// stack bound, so a non-zero count would mean the bound is
		// wrong rather than the shape is hard.
		uint32_t over = 1, flat = 1;
		vidterm_fill_stats(&over, &flat);
		CHECK(over == 0);
		CHECK(flat == 0);
	}

	// Curves. The properties worth asserting are the ones a wrong
	// algorithm gets wrong: that the outline is HOLLOW and the fill is
	// not, that both are centred on the draw pointer, and that the
	// extremes land exactly a radius away rather than one pixel out.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// Circle of radius 40 centred at 200,200.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, ESC); v.write(DATA, 0x50);
		v.write(DATA, 0); v.write(DATA, 40);
		settle(v);

		CHECK(video_framebuffer[200][240] != 0x00);  // right extreme
		CHECK(video_framebuffer[200][160] != 0x00);  // left
		CHECK(video_framebuffer[240][200] != 0x00);  // bottom
		CHECK(video_framebuffer[160][200] != 0x00);  // top
		CHECK(video_framebuffer[200][200] == 0x00);  // HOLLOW centre
		CHECK(video_framebuffer[200][241] == 0x00);  // nothing outside

		// The filled form of the same circle fills its centre.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 1); v.write(DATA, 0x2C);	// 300,200
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, ESC); v.write(DATA, 0x53);
		v.write(DATA, 0); v.write(DATA, 40);
		settle(v);
		CHECK(video_framebuffer[200][300] != 0x00);  // centre IS filled
		CHECK(video_framebuffer[200][340] != 0x00);  // out to the edge
		CHECK(video_framebuffer[200][342] == 0x00);  // and no further

		// An ellipse is wider than it is tall when told to be, which
		// is the check a circle routine wearing an ellipse's name
		// would fail.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 1); v.write(DATA, 0x90);	// 400,350
		v.write(DATA, 1); v.write(DATA, 0x5E);
		v.write(DATA, ESC); v.write(DATA, 0x54);
		v.write(DATA, 0); v.write(DATA, 60);	// rx 60
		v.write(DATA, 0); v.write(DATA, 20);	// ry 20
		settle(v);
		CHECK(video_framebuffer[350][455] != 0x00);  // 55 px right: in
		CHECK(video_framebuffer[350][400] != 0x00);  // centre filled
		// The vertical extreme is EXACTLY ry away and is part of the
		// shape; one pixel beyond it is not. Checking both sides of
		// that line is what catches an off-by-one, where checking
		// only "somewhere outside" would not.
		CHECK(video_framebuffer[330][400] != 0x00);  // ry up: the edge
		CHECK(video_framebuffer[329][400] == 0x00);  // one further: out
		// And it is genuinely wider than tall: 55 right is inside
		// while 25 up is already outside.
		CHECK(video_framebuffer[325][400] == 0x00);
	}

	// VID-025: text must not hide graphics, and erasing text must put the
	// graphics BACK rather than leave a hole. Both halves matter -- a
	// transparent background that never erases just ghosts, which is why
	// the console could not use transparency before the shadow existed.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);		// claim and clear both layers
		settle(v);

		// A bar across the middle of the screen, in the foreground
		// colour -- whatever the palette makes that.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 64);	// SetDPtr 64,128
		v.write(DATA, 0); v.write(DATA, 128);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 1); v.write(DATA, 0x90);	// Bar to 400,300
		v.write(DATA, 1); v.write(DATA, 44);
		settle(v);

		const uint8_t gfx = video_framebuffer[168][84];
		CHECK(gfx != 0x00);		// the bar really is there

		// Put a character on top of it, at cell 10,10 = px 80,160.
		v.write(DATA, 0x02);
		v.write(DATA, (uint8_t)(10 + 0x20));
		v.write(DATA, (uint8_t)(10 + 0x20));
		v.write(DATA, 'A');
		settle(v);

		// The glyph is drawn, and the graphics still shows through
		// the gaps: row 0 of 'A' is blank in this font, so the whole
		// of that row must still be the bar's colour.
		bool any_fg = false;
		for (int gx = 0; gx < CHAR_W; gx++)
			if (video_framebuffer[160 + 8][80 + gx] == 0xFF)
				any_fg = true;
		CHECK(any_fg);			// the character is visible
		CHECK(video_framebuffer[160][80] == gfx);   // and so is the bar
		CHECK(video_framebuffer[160][87] == gfx);

		// Now erase it. The cell must return to the BAR, not to black:
		// painting a background here is what used to punch a hole in
		// the graphics.
		v.write(DATA, 0x02);
		v.write(DATA, (uint8_t)(10 + 0x20));
		v.write(DATA, (uint8_t)(10 + 0x20));
		v.write(DATA, ' ');
		settle(v);
		for (int gy = 0; gy < CHAR_H; gy++)
			for (int gx = 0; gx < CHAR_W; gx++)
				CHECK(video_framebuffer[160 + gy][80 + gx] == gfx);
	}

	// Fills take two routes now: a full-width rectangle is one contiguous
	// run, anything narrower is one run per row. The fast path is the one
	// a cleared screen takes, so it needs its own check -- a length
	// computed wrongly there would overrun into the rows below.
	{
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// Root window, so the clip is the whole panel: a bar from 0,0
		// to the far corner takes the contiguous path.
		vidterm_fill_rect(0, 100, FB_WIDTH, 10, 0x1C);
		CHECK(video_framebuffer[100][0]   == 0x1C);
		CHECK(video_framebuffer[109][1023] == 0x1C);  // last row, last px
		CHECK(video_framebuffer[99][0]   != 0x1C);    // not the row above
		CHECK(video_framebuffer[110][0]  != 0x1C);    // nor the one below

		// A narrower rectangle takes the per-row path and must not
		// bleed past its right edge into the next row's left.
		vidterm_fill_rect(10, 200, 20, 3, 0x1C);
		CHECK(video_framebuffer[200][10] == 0x1C);
		CHECK(video_framebuffer[202][29] == 0x1C);
		CHECK(video_framebuffer[200][30] != 0x1C);    // stops at the edge
		CHECK(video_framebuffer[203][10] != 0x1C);
	}

	// Region text operations -- the primitives a text window is built from.
	// The property that matters is CONTAINMENT: what is inside the
	// rectangle changes and what is outside does not. A region routine that
	// quietly spans the full row width would pass any check that only
	// looked inside it, so every case below asserts the neighbours too.
	{
		// 0xE0 is the console's own attribute (white on opaque black),
		// which is what check_cell compares the rendered pixels
		// against; the grid alone proves nothing until it is blitted.
		vidterm_clear('#', 0xE0);
		vidterm_clear_region(10, 5, 4, 3, '.', 0xE0);
		vidterm_refresh();

		for (int r = 5; r < 8; r++)
			for (int c = 10; c < 14; c++)
				CHECK(check_cell(c, r, '.'));
		CHECK(check_cell(9, 5, '#'));	// column before
		CHECK(check_cell(14, 5, '#'));	// column after
		CHECK(check_cell(10, 4, '#'));	// row above
		CHECK(check_cell(10, 8, '#'));	// row below

		// Scroll that region up one row: its rows move, its last row
		// fills, and the cells beside it stay put.
		vidterm_clear_region(10, 5, 4, 1, 'A', 0xE0);
		vidterm_clear_region(10, 6, 4, 1, 'B', 0xE0);
		vidterm_clear_region(10, 7, 4, 1, 'C', 0xE0);
		vidterm_scroll_region(10, 5, 4, 3, ' ', 0xE0);
		vidterm_refresh();
		CHECK(check_cell(10, 5, 'B'));
		CHECK(check_cell(10, 6, 'C'));
		CHECK(check_cell(10, 7, ' '));	// vacated row filled
		CHECK(check_cell(9, 5, '#'));	// neighbours untouched
		CHECK(check_cell(14, 6, '#'));
	}

	// Window sub-code framing: DWSet takes EIGHT params and Select ONE,
	// both leading with a window id -- a deliberate divergence from CoCo3,
	// where identity rides on the device path and never reaches the byte
	// stream. Framing is the part that must be right before anything is
	// implemented: a wrong parameter count does not draw the wrong thing,
	// it misparses every byte that follows. Printable params make that
	// visible -- if the count were short, a leftover param would render as
	// a glyph and displace what comes after it.
	{
		v.write(DATA, 0x0C);		// clear and home
		settle(v);

		v.write(DATA, 0x1B);
		v.write(DATA, 0x20);		// DWSet
		for (uint8_t p = 'A'; p <= 'H'; p++)	// 8 params
			v.write(DATA, p);
		v.write(DATA, 'Z');
		settle(v);
		CHECK(check_cell(0, 0, 'Z'));	// all eight swallowed
		CHECK(!check_cell(0, 0, 'H'));	// not one left over

		v.write(DATA, 0x0C);
		settle(v);

		v.write(DATA, 0x1B);
		v.write(DATA, 0x21);		// Select
		v.write(DATA, 'A');		// 1 param: the window id
		v.write(DATA, 'Z');
		settle(v);
		CHECK(check_cell(0, 0, 'Z'));	// the id was swallowed
	}

	// The render drain is BOUNDED per call. It used to run until the FIFO
	// was empty, so one call could work through a full 2 KB backlog --
	// 18 ms measured on hardware, longer than a frame, on the core that
	// owes the overlay service. Sabotage check: remove the budget from
	// vid_link_render() and the first CHECK below fails, because the
	// whole burst lands in a single tick.
	{
		v.write(STAT, VID_CTL_RESET);	// drop loopback, flush
		settle(v);
		v.write(DATA, 0x0C);		// clear and home
		settle(v);

		// TEXT_COLS is 128, so exactly VID_LINK_DRAIN_MAX characters
		// fill two rows and the next one starts row 2. That cell is
		// the probe: it can only be drawn by a SECOND drain.
		for (unsigned i = 0; i < VID_LINK_DRAIN_MAX + 1u; i++)
			v.write(DATA, 'A');

		v.tick(1);
		CHECK(!check_cell(0, 2, 'A'));	// bound held: one short
		CHECK(check_cell(0, 0, 'A'));	// but the burst did render
		CHECK(check_cell(127, 1, 'A'));	// right up to the bound

		v.tick(1);
		CHECK(check_cell(0, 2, 'A'));	// and nothing was dropped
	}

	// The TEXT REFRESH is bounded too: at most VIDTERM_REFRESH_ROWS_MAX text
	// rows per render pass. A full-screen transparent refresh over drawn
	// graphics measured 28.7 ms on hardware -- 1.7 frames, and no faster
	// blitter can bring it under one -- so a full-screen change now finishes
	// over a few passes instead of one pass arriving late (agreed 2026-09-26).
	//
	// Three things must hold, and each is checked with single passes:
	//   1. one pass draws only part of a full-screen change;
	//   2. while drawing is owed, NEW BYTES ARE NOT CONSUMED -- otherwise a
	//      later burst's graphics could land before an earlier burst's text
	//      was drawn and opaque text would paint over them;
	//   3. it finishes within the passes the bound implies, and the owed
	//      byte is then taken.
	// A scroll from the bottom row dirties all 30 rows in one short burst.
	{
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);			// clear and home
		settle(v);
		v.write(DATA, 0x02);			// cursor to col 0, row 29
		v.write(DATA, 0x20);
		v.write(DATA, 0x20 + TEXT_ROWS - 1);
		v.write(DATA, 'X');
		v.write(DATA, 0x0A);			// LF at the bottom: scroll
		v.tick(1);

		// After the scroll the X sits on row 28. Its pixels are drawn by
		// the refresh, which one pass cannot have reached.
		auto x_drawn = [&]() {
			int lit = 0;
			for (int gy = 0; gy < CHAR_H; gy++)
				for (int gx = 0; gx < CHAR_W; gx++)
					if (video_framebuffer[(TEXT_ROWS - 2) * CHAR_H
					    + gy][gx] == 0xFFu)
						lit++;
			return lit > 4;
		};
		// The TEXT GRID, read directly: check_cell() looks at pixels,
		// and "not drawn yet" is exactly what this pass is meant to
		// leave, so it cannot say whether the grid itself moved.
		auto grid = [&](int col, int row) {
			uint8_t cell[2];
			vidterm_read_row(col, row, 1, cell);
			return cell[0];
		};
		CHECK(grid(0, TEXT_ROWS - 2) == 'X');	// the grid scrolled
		CHECK(!x_drawn());			// 1: its pixels not yet
		CHECK(!vid_link_idle());		// drawing is owed

		// 2: a byte written now must NOT be consumed until the drawing
		// is finished. Checked in the grid, where consumption shows at
		// once, not in pixels, where it would not show anyway. LF keeps
		// the column, so after 'X' and the scroll the cursor is at col 1.
		// (A first draft checked col 0 in pixels -- a check that passed
		// whatever the code did.)
		v.write(DATA, 'Z');
		v.tick(1);
		CHECK(grid(1, TEXT_ROWS - 1) != 'Z');

		// 3: done within the passes the bound implies (30 rows at 5 a
		// pass is 6, plus the one that takes the 'Z'), and not before
		// the refresh -- the X is drawn, then the Z is consumed.
		int passes = 2;
		while (!vid_link_idle() && passes < 32) {
			v.tick(1);
			passes++;
		}
		CHECK(x_drawn());
		CHECK(grid(1, TEXT_ROWS - 1) == 'Z');	// taken once drawing done
		CHECK(passes <= (TEXT_ROWS + VIDTERM_REFRESH_ROWS_MAX - 1) /
		    VIDTERM_REFRESH_ROWS_MAX + 2);
		CHECK(passes > 2);	// it really was spread out
	}

	// A GRID of stamps, which is what a guest actually does with PutBlk --
	// and the case no other test here covers. Every existing block test
	// stamps once or twice; a fault that only appears after N stamps, or
	// only in the last row, or only past some coordinate, would pass all
	// of them. Hardware showed exactly that shape: a page of gems where
	// the early stamps were right and the ones at bottom right were not.
	//
	// The image is deliberately NOT a smooth ramp. A ramp of row*32+col
	// aliases under any shift where dy*32+dx is a multiple of 256, so a
	// purely vertical slip of eight rows would leave it pixel-identical
	// and the test would pass while the picture was wrong. (That trap has
	// already cost a day on this renderer once, with a diagonal weave
	// invariant under every shift summing to zero mod 8.) So the image
	// carries four DIFFERENT corner markers plus one off-centre mark: the
	// corners pin position, their disagreement catches shear, and the
	// off-centre mark denies the whole thing any rotational symmetry to
	// hide behind.
	{
		static const uint8_t ESC = 0x1B;
		static const int GW = 32, GH = 32;
		static const int COLS = 10, ROWS = 4;
		static const int X0 = 20, Y0 = 300;
		static const int XS = 100, YS = 40;
		static const uint8_t BODY = 0x20;
		static const uint8_t TL = 0xFF, TR = 0xFE;
		static const uint8_t BL = 0xFD, BR = 0xFC, ODD = 0xF0;

		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		uint8_t img[GW * GH];
		for (int i = 0; i < GW * GH; i++)
			img[i] = BODY;
		img[0] = TL;
		img[GW - 1] = TR;
		img[(GH - 1) * GW] = BL;
		img[(GH - 1) * GW + GW - 1] = BR;
		img[5 * GW + 1] = ODD;

		// DefGPB group 2 buffer 1, 1024 bytes.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 2); v.write(DATA, 1);
		v.write(DATA, 0x04); v.write(DATA, 0x00);
		// GPLoad: type 1, 32x32, 1024 bytes.
		v.write(DATA, ESC); v.write(DATA, 0x2B);
		v.write(DATA, 2); v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, (uint8_t)GW);
		v.write(DATA, 0); v.write(DATA, (uint8_t)GH);
		v.write(DATA, 0x04); v.write(DATA, 0x00);
		// Drained every 128 bytes: the command FIFO is 2 KB and this
		// payload is 1 KB, so writing it in one go without letting the
		// render side consume overflows it -- which looks exactly like
		// a protocol bug and is not one.
		for (int i = 0; i < GW * GH; i++) {
			v.write(DATA, img[i]);
			if ((i & 127) == 127)
				settle(v);
		}
		settle(v);

		// Stamp the grid, rows outermost, exactly as a guest loop would.
		for (int r = 0; r < ROWS; r++)
			for (int c = 0; c < COLS; c++) {
				const int x = X0 + c * XS, y = Y0 + r * YS;

				v.write(DATA, ESC); v.write(DATA, 0x2D);
				v.write(DATA, 2); v.write(DATA, 1);
				v.write(DATA, (uint8_t)(x >> 8));
				v.write(DATA, (uint8_t)(x & 0xFF));
				v.write(DATA, (uint8_t)(y >> 8));
				v.write(DATA, (uint8_t)(y & 0xFF));
				settle(v);
			}
		settle(v);

		// Now check every single one, and say WHICH failed rather than
		// just that something did -- "the last eight" versus "all of
		// them" versus "one" are three different bugs, and a bare pass
		// or fail cannot tell them apart.
		int bad = 0;
		for (int r = 0; r < ROWS; r++)
			for (int c = 0; c < COLS; c++) {
				const int x = X0 + c * XS, y = Y0 + r * YS;
				bool ok = true;

				ok &= video_framebuffer[y][x] == TL;
				ok &= video_framebuffer[y][x + GW - 1] == TR;
				ok &= video_framebuffer[y + GH - 1][x] == BL;
				ok &= video_framebuffer[y + GH - 1][x + GW - 1]
				      == BR;
				ok &= video_framebuffer[y + 5][x + 1] == ODD;
				// The body, sampled away from every marker.
				ok &= video_framebuffer[y + 16][x + 16] == BODY;
				// And nothing written PAST the block: a stride
				// or size error spills to the right, into the
				// gap the grid leaves between columns.
				ok &= video_framebuffer[y][x + GW] != TL;
				ok &= video_framebuffer[y][x + GW] != BODY;
				if (!ok) {
					bad++;
					fprintf(stderr, "  gem r%d c%d at "
						"%d,%d misplaced\n", r, c, x, y);
				}
			}
		CHECK(bad == 0);

		// The same grid stamped again must be idempotent. A stamp that
		// depended on what was already there -- a mix mode left set, a
		// pattern flag left on by an earlier fill -- would differ on
		// the second pass, and that is the failure mode that made the
		// hardware page wrong twice already.
		for (int r = 0; r < ROWS; r++)
			for (int c = 0; c < COLS; c++) {
				const int x = X0 + c * XS, y = Y0 + r * YS;

				v.write(DATA, ESC); v.write(DATA, 0x2D);
				v.write(DATA, 2); v.write(DATA, 1);
				v.write(DATA, (uint8_t)(x >> 8));
				v.write(DATA, (uint8_t)(x & 0xFF));
				v.write(DATA, (uint8_t)(y >> 8));
				v.write(DATA, (uint8_t)(y & 0xFF));
				settle(v);
			}
		settle(v);
		int bad2 = 0;
		for (int r = 0; r < ROWS; r++)
			for (int c = 0; c < COLS; c++) {
				const int x = X0 + c * XS, y = Y0 + r * YS;

				if (video_framebuffer[y][x] != TL ||
				    video_framebuffer[y + GH - 1][x + GW - 1]
				    != BR)
					bad2++;
			}
		CHECK(bad2 == 0);
	}

	// Capture a region OVER the grid and re-stamp it -- the tail of the
	// same hardware page, and the seam the grid test above does not reach.
	// GetBlk reads the graphics SHADOW while the grid test checked the
	// FRAMEBUFFER, so the two together are what says those agree. If a
	// stamp wrote pixels to one and not the other, or wrote them at
	// different coordinates, the grid would look perfect and everything
	// captured from it would come back displaced -- which is exactly how
	// "the gems are misplaced" would present while every gem was in fact
	// drawn in the right place.
	//
	// The region is deliberately 120x120 over a grid on a 100x40 pitch,
	// so it straddles several stamps and lands on no boundary: a capture
	// aligned to the pitch could be off by a whole cell and still compare
	// equal.
	{
		static const uint8_t ESC = 0x1B;
		static const int RW = 120, RH = 120;
		static const int SX = 170, SY = 330;	// on no gem boundary
		static const int DX = 60,  DY = 60;

		// Snapshot the source region before the copy, because the
		// destination overlaps nothing but the source must not move.
		uint8_t want[RH][RW];
		for (int r = 0; r < RH; r++)
			for (int c = 0; c < RW; c++)
				want[r][c] = video_framebuffer[SY + r][SX + c];

		// The region has to contain some gem, or this proves nothing
		// but that background copies to background.
		int marks = 0;
		for (int r = 0; r < RH; r++)
			for (int c = 0; c < RW; c++)
				if (want[r][c] == 0xFF || want[r][c] == 0xFC)
					marks++;
		CHECK(marks >= 4);

		// DefGPB group 2 buffer 2, 120*120 = 14400 bytes.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 2); v.write(DATA, 2);
		v.write(DATA, (uint8_t)((RW * RH) >> 8));
		v.write(DATA, (uint8_t)((RW * RH) & 0xFF));
		// GetBlk the region.
		v.write(DATA, ESC); v.write(DATA, 0x2C);
		v.write(DATA, 2); v.write(DATA, 2);
		v.write(DATA, (uint8_t)(SX >> 8)); v.write(DATA, (uint8_t)SX);
		v.write(DATA, (uint8_t)(SY >> 8)); v.write(DATA, (uint8_t)SY);
		v.write(DATA, (uint8_t)(RW >> 8)); v.write(DATA, (uint8_t)RW);
		v.write(DATA, (uint8_t)(RH >> 8)); v.write(DATA, (uint8_t)RH);
		settle(v);
		// PutBlk it somewhere that does not overlap the source.
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 2); v.write(DATA, 2);
		v.write(DATA, (uint8_t)(DX >> 8)); v.write(DATA, (uint8_t)DX);
		v.write(DATA, (uint8_t)(DY >> 8)); v.write(DATA, (uint8_t)DY);
		settle(v);

		// Pixel for pixel, and report the FIRST disagreement with both
		// coordinates: a uniform offset and a shear look identical in a
		// count, and the offset is the number that names the bug.
		int diff = 0, fr = -1, fc = -1;
		for (int r = 0; r < RH; r++)
			for (int c = 0; c < RW; c++)
				if (video_framebuffer[DY + r][DX + c] !=
				    want[r][c]) {
					if (diff == 0) { fr = r; fc = c; }
					diff++;
				}
		if (diff != 0)
			fprintf(stderr, "  region copy: %d px differ, first at "
				"+%d,+%d (want %02X got %02X)\n", diff, fc, fr,
				want[fr][fc],
				video_framebuffer[DY + fr][DX + fc]);
		CHECK(diff == 0);

		// And the source is still there: a capture that read through
		// to the framebuffer could have been damaged by its own write.
		int moved = 0;
		for (int r = 0; r < RH; r++)
			for (int c = 0; c < RW; c++)
				if (video_framebuffer[SY + r][SX + c] !=
				    want[r][c])
					moved++;
		CHECK(moved == 0);
	}

	// PutBlk UNDER A MIX MODE. This exists because the ordinary stamp is
	// now a bulk row copy -- one memcpy in from the buffer, one out to the
	// shadow -- while a mix mode makes every pixel depend on what is
	// already there, so it must still take the per-pixel path. That split
	// means there are two routes through PutBlk where there was one, and
	// the bulk one cannot honour XOR by construction the way the old
	// per-pixel loop did. Nothing here previously combined LSet with
	// PutBlk, so the slower route was reachable and untested: precisely
	// the shape of gap that lets an optimisation quietly change meaning.
	{
		static const uint8_t ESC = 0x1B;

		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		// A 4x4 buffer of a single known value.
		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 3); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 16);
		v.write(DATA, ESC); v.write(DATA, 0x2B);
		v.write(DATA, 3); v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 4);
		v.write(DATA, 0); v.write(DATA, 4);
		v.write(DATA, 0); v.write(DATA, 16);
		for (int i = 0; i < 16; i++)
			v.write(DATA, 0x0F);
		settle(v);

		// Lay a known background down with a bar, so the mix has
		// something real to combine against.
		v.write(DATA, ESC); v.write(DATA, 0x32); v.write(DATA, 0x33);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, ESC); v.write(DATA, 0x4B);
		v.write(DATA, 0); v.write(DATA, 210);
		v.write(DATA, 0); v.write(DATA, 210);
		settle(v);
		const uint8_t bg = video_framebuffer[202][202];
		CHECK(bg != 0x0F);	// or the XOR below proves nothing

		// Replace mode first: the stamp lands verbatim.
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 3); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 202);
		v.write(DATA, 0); v.write(DATA, 202);
		settle(v);
		CHECK(video_framebuffer[202][202] == 0x0F);
		CHECK(video_framebuffer[205][205] == 0x0F);

		// Put the background back, then XOR the same stamp over it.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, ESC); v.write(DATA, 0x4B);
		v.write(DATA, 0); v.write(DATA, 210);
		v.write(DATA, 0); v.write(DATA, 210);
		settle(v);
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 3);
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 3); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 202);
		v.write(DATA, 0); v.write(DATA, 202);
		settle(v);
		// XOR combines against the GRAPHICS PLANE, which is what the
		// bar wrote, so the result is a value that is neither operand.
		CHECK(video_framebuffer[202][202] == (uint8_t)(bg ^ 0x0F));
		CHECK(video_framebuffer[205][205] == (uint8_t)(bg ^ 0x0F));

		// And XOR twice is identity -- the property that says the mix
		// really read what was there rather than just writing
		// something different.
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 3); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 202);
		v.write(DATA, 0); v.write(DATA, 202);
		settle(v);
		CHECK(video_framebuffer[202][202] == bg);
		CHECK(video_framebuffer[205][205] == bg);

		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 0);
		settle(v);
	}

	// A PANEL-WIDE FILL MUST STILL BE VISIBLE TO GetBlk. The fill now
	// short-circuits: covering the whole panel makes the graphics shadow
	// uniform by definition, so it sets one colour and marks every row
	// implicit instead of writing ~960 KB across the QMI (measured at
	// 24 ms on hardware, one and a half frames). That is only valid if
	// "uniform" and "filled" are indistinguishable to every reader of the
	// shadow -- and GetBlk is the reader that would notice, because it is
	// the one that copies the plane out rather than merely compositing
	// against it.
	//
	// So: fill the panel, capture a block from the middle of it, and every
	// byte must be the fill colour. If the short-circuit ever diverges
	// from the fill it replaced, this is where it shows.
	{
		static const uint8_t ESC = 0x1B;
		static const uint8_t C = 0x6Du;	// not 0 and not the default
		static const int RW = 40, RH = 8;

		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);

		vidterm_set_origin(0, 0);
		vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
		vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, C);

		// The framebuffer obviously holds it; the shadow is the question.
		CHECK(video_framebuffer[0][0] == C);
		CHECK(video_framebuffer[FB_HEIGHT - 1][FB_WIDTH - 1] == C);

		v.write(DATA, ESC); v.write(DATA, 0x29);
		v.write(DATA, 4); v.write(DATA, 1);
		v.write(DATA, (uint8_t)((RW * RH) >> 8));
		v.write(DATA, (uint8_t)((RW * RH) & 0xFF));
		v.write(DATA, ESC); v.write(DATA, 0x2C);
		v.write(DATA, 4); v.write(DATA, 1);
		v.write(DATA, 1); v.write(DATA, 0x00);	// x 256
		v.write(DATA, 0); v.write(DATA, 200);	// y 200
		v.write(DATA, 0); v.write(DATA, (uint8_t)RW);
		v.write(DATA, 0); v.write(DATA, (uint8_t)RH);
		settle(v);
		// Put it back somewhere else: what comes out of the buffer is
		// what the shadow gave it, so the destination reveals it.
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 4); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 8);	// x 8
		v.write(DATA, 0); v.write(DATA, 8);	// y 8
		settle(v);
		int wrong = 0;
		for (int r = 0; r < RH; r++)
			for (int c = 0; c < RW; c++)
				if (video_framebuffer[8 + r][8 + c] != C)
					wrong++;
		if (wrong != 0)
			fprintf(stderr, "  panel fill: %d of %d captured bytes"
				" were not the fill colour (got %02X)\n", wrong,
				RW * RH, video_framebuffer[8][8]);
		CHECK(wrong == 0);

		// And a PART-panel fill afterwards must not be swallowed by the
		// same shortcut: it has to reach the shadow for real, or the
		// panel-wide case would have broken every smaller fill.
		vidterm_fill_rect(100, 100, 64, 16, 0x24u);
		v.write(DATA, ESC); v.write(DATA, 0x2C);
		v.write(DATA, 4); v.write(DATA, 1);
		v.write(DATA, 0); v.write(DATA, 100);	// x 100
		v.write(DATA, 0); v.write(DATA, 100);	// y 100
		v.write(DATA, 0); v.write(DATA, (uint8_t)RW);
		v.write(DATA, 0); v.write(DATA, (uint8_t)RH);
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 4); v.write(DATA, 1);
		v.write(DATA, 1); v.write(DATA, 0x90);	// x 400
		v.write(DATA, 0); v.write(DATA, 8);	// y 8
		settle(v);
		CHECK(video_framebuffer[8][400] == 0x24u);
	}

	// TRANSPARENT TEXT OVER A COLOURED BUT UNDRAWN PLANE. A panel-wide fill
	// now leaves every shadow row implicit, holding its colour as a single
	// "uniform" value rather than as bytes. A transparent cell over such a
	// row must show THAT colour behind its glyph -- not the cell's own
	// background, and not whatever colour the tests happen to clear to.
	//
	// Every earlier text test cleared the plane to the same default a
	// transparent attr resolves to, so the two were indistinguishable: a
	// blitter using the wrong one passed the whole suite. Found by mutation
	// when the blitter was rewritten word-wise. So use a colour that is
	// neither, and check the undrawn (implicit) and drawn (resident) cases
	// give the SAME answer -- they must, since both mean "the plane is C".
	{
		static const uint8_t C = 0x6Du;
		static const uint8_t WHITE_TRANSP = 0xE1u;	// fg white, bit0 set
		const int col = 10, row = 5;
		const int x = col * CHAR_W, y = row * CHAR_H;

		for (int pass = 0; pass < 2; pass++) {
			v.write(STAT, VID_CTL_RESET);
			settle(v);
			vidterm_set_origin(0, 0);
			vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
			if (pass == 0) {
				// Implicit: one panel-wide fill, no row real.
				vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, C);
			} else {
				// Resident: two part-height bars make every
				// row real, holding the same colour as bytes.
				vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT / 2, C);
				vidterm_fill_rect(0, FB_HEIGHT / 2, FB_WIDTH,
				    FB_HEIGHT - FB_HEIGHT / 2, C);
			}
			vidterm_putch(col, row, 'M', WHITE_TRANSP);
			vidterm_putch(col + 1, row, ' ', WHITE_TRANSP);
			vidterm_refresh();

			// This face leaves the rightmost column of every glyph
			// blank, so it is background in every row of the 'M'.
			int bad_bg = 0, fg_seen = 0;
			for (int gy = 0; gy < CHAR_H; gy++) {
				if (video_framebuffer[y + gy][x + CHAR_W - 1] != C)
					bad_bg++;
				for (int gx = 0; gx < CHAR_W; gx++) {
					if (video_framebuffer[y + gy][x + CHAR_W +
					    gx] != C)
						bad_bg++;	// the space
					if (video_framebuffer[y + gy][x + gx] ==
					    0xFFu)
						fg_seen++;
				}
			}
			if (bad_bg != 0)
				fprintf(stderr, "  transparent over %s plane: %d"
				    " background pixels not the plane colour"
				    " (got %02X)\n", pass ? "resident" :
				    "implicit", bad_bg,
				    video_framebuffer[y][x + CHAR_W - 1]);
			CHECK(bad_bg == 0);
			CHECK(fg_seen > 10);	// the glyph really drew
		}
	}

	// THE TEXT BLITTER AGAINST AN ORACLE, over a plane that varies in BOTH
	// directions. The refresh reads the shadow a pixel row at a time
	// across a run of cells (so its PSRAM reads are consecutive), which
	// means a cell's background now comes out of a buffer shared with its
	// neighbours -- an off-by-one-cell there, or a pixel row taken from
	// the wrong text row, would restore graphics that is merely nearby.
	// Every earlier transparent-text test used a plane uniform along each
	// row, where a sideways slip is invisible. The plane here is chosen
	// AGAINST the blitter's symmetries: it changes under a shift of one
	// cell (8), one word (4) and one text row (16).
	//
	// The cells mix everything a run can hold: transparent and opaque,
	// solid, blank and ordinary glyphs, dense runs and isolated cells
	// (including either side of a 64-pixel boundary and the last column),
	// over text rows wholly drawn, wholly undrawn, and split between the
	// two. Every pixel of the panel is checked, so a write to a cell that
	// was not dirty is caught too. Then again through the bounded refresh.
	{
		static uint8_t model[FB_HEIGHT][FB_WIDTH];
		static const uint8_t U = 0x92u;		// undrawn rows' colour
		auto plane = [](int x, int y) {
			return (uint8_t)(x * 37 + y * 101 + (x >> 3) * 11);
		};
		auto fg_of = [](uint8_t a) {
			return (uint8_t)(((a & 0x80) ? 0xE0 : 0) |
			    ((a & 0x40) ? 0x1C : 0) | ((a & 0x20) ? 0x03 : 0));
		};
		auto bg_of = [](uint8_t a) {
			return (uint8_t)(((a & 0x08) ? 0x40 : 0) |
			    ((a & 0x04) ? 0x08 : 0) | ((a & 0x02) ? 0x01 : 0));
		};
		static const uint8_t CHARS[] = { 'A', 'M', ' ', 0xDB, 'g', '#',
		    '@', '.', 'W', 0xB1 };
		static const uint8_t ATTRS[] = { 0xE1, 0x21, 0xE0, 0x4A, 0xC1,
		    0x9C, 0x61 };
		static const int SPARSE[] = { 50, 57, 63, 64, 71, 100, 127 };

		for (int bounded = 0; bounded < 2; bounded++) {
			v.write(STAT, VID_CTL_RESET);
			settle(v);
			vidterm_set_pattern(0, 0);
			vidterm_set_logic(0);
			vidterm_set_origin(0, 0);
			vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
			vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, U);
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					model[y][x] = U;
			// Pixel rows 40..119: text row 2 is half drawn, 3..6
			// wholly, 7 half, 8 not at all.
			for (int y = 40; y < 120; y++)
				for (int x = 0; x < FB_WIDTH; x++) {
					vidterm_plot(x, y, plane(x, y));
					model[y][x] = plane(x, y);
				}
			// Show the plane itself everywhere first, so any cell
			// the refresh should not touch has a known value.
			(void)vidterm_clear(' ', 0x01u);
			vidterm_refresh();

			auto place = [&](int col, int row, int k) {
				const uint8_t ch = CHARS[k % sizeof(CHARS)];
				const uint8_t at = ATTRS[k % sizeof(ATTRS)];

				vidterm_putch(col, row, ch, at);
				for (int gy = 0; gy < CHAR_H; gy++) {
					const uint8_t m = font_spleen_8x16[ch][gy];
					const int py = row * CHAR_H + gy;

					for (int gx = 0; gx < CHAR_W; gx++) {
						const int px = col * CHAR_W + gx;
						const uint8_t b = (at & 1) ?
						    (py >= 40 && py < 120 ?
						    plane(px, py) : U) : bg_of(at);

						model[py][px] = (m & (0x80 >> gx)) ?
						    fg_of(at) : b;
					}
				}
			};
			int k = 0;
			for (int row = 2; row <= 8; row++) {
				for (int col = 0; col <= 40; col++)
					place(col, row, k++);
				for (int col : SPARSE)
					place(col, row, k++ + row);
			}

			if (bounded) {
				int passes = 0;
				while (vidterm_refresh_bounded())
					passes++;
				CHECK(passes > 0);	// it really was split
			} else {
				vidterm_refresh();
			}

			int bad = 0, fx = -1, fy = -1;
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					if (video_framebuffer[y][x] != model[y][x]) {
						if (!bad) { fx = x; fy = y; }
						bad++;
					}
			if (bad)
				fprintf(stderr, "  text oracle%s: %d px differ,"
				    " first %d,%d (got %02X want %02X)\n",
				    bounded ? " (bounded)" : "", bad, fx, fy,
				    video_framebuffer[fy][fx], model[fy][fx]);
			CHECK(bad == 0);
		}
	}

	// THE COMMAND TRACE: what the renderer was told, byte by byte, with
	// the role each byte played. Its whole purpose is to settle "did we
	// draw it wrong, or were we told to draw it there?" -- a question that
	// cost three rounds and two wrong diagnoses on one wgfx page. So the
	// test is not only that bytes are recorded, but that a LOST byte is
	// visible in the record.
	{
		static const uint8_t ESC = 0x1B;
		const uint8_t S = VID_BYTE_START, O = VID_BYTE_SUBCODE,
		    P = VID_BYTE_PARAM, Y = VID_BYTE_PAYLOAD;
		uint32_t oldest, next;

		v.write(STAT, VID_CTL_RESET);
		settle(v);
		vid_link_trace_span(&oldest, &next);
		const uint32_t base = next;

		// Text, a cursor move, a well-formed PutBlk, text.
		const uint8_t seq[] = { 'A', 0x02, 0x25, 0x2A,
		    ESC, 0x2D, 0x01, 0x01, 0x00, 0x28, 0x00, 0x3C, 'B' };
		const uint8_t cls[] = { S, S, P, P,
		    S, O, P, P, P, P, P, P, S };
		for (uint8_t b : seq)
			v.write(DATA, b);
		settle(v);

		vid_link_trace_span(&oldest, &next);
		CHECK(next == base + sizeof(seq));	// offsets are "cons"
		uint8_t gb[64] = { 0 }, gc[64] = { 0 };	// zeroed: a short read fails the checks below rather than reading garbage
		uint32_t got = vid_link_trace_read(base, sizeof(seq), gb, gc);
		CHECK(got == sizeof(seq));
		int wrong = 0;
		for (unsigned i = 0; i < sizeof(seq); i++)
			if (gb[i] != seq[i] || gc[i] != cls[i]) {
				fprintf(stderr, "  trace[%u]: got %02X/%u want"
				    " %02X/%u\n", i, gb[i], gc[i], seq[i],
				    cls[i]);
				wrong++;
			}
		CHECK(wrong == 0);

		// GPLoad's streamed data is PAYLOAD, and the stream is back in
		// step afterwards: the byte after the payload starts afresh.
		const uint8_t load[] = { ESC, 0x29, 5, 1, 0, 4,		// DefGPB
		    ESC, 0x2B, 5, 1, 1, 0, 2, 0, 2, 0, 4,		// GPLoad
		    0x11, 0x22, 0x33, 0x44, 'C' };
		const uint8_t lcls[] = { S, O, P, P, P, P,
		    S, O, P, P, P, P, P, P, P, P, P,
		    Y, Y, Y, Y, S };
		const uint32_t lbase = next;
		for (uint8_t b : load)
			v.write(DATA, b);
		settle(v);
		got = vid_link_trace_read(lbase, sizeof(load), gb, gc);
		CHECK(got == sizeof(load));
		wrong = 0;
		for (unsigned i = 0; i < sizeof(load); i++)
			if (gb[i] != load[i] || gc[i] != lcls[i])
				wrong++;
		CHECK(wrong == 0);

		// A LOST BYTE. A PutBlk missing one coordinate byte, then text:
		// the 'D' is consumed as the missing sixth parameter. The trace
		// must show exactly that -- a printable character marked as a
		// PARAMETER -- because that mark is how a dropped byte is found.
		vid_link_trace_span(&oldest, &next);
		const uint32_t dbase = next;
		const uint8_t lost[] = { ESC, 0x2D, 0x05, 0x01, 0x00, 0x28,
		    0x00, 'D', 'E' };
		for (uint8_t b : lost)
			v.write(DATA, b);
		settle(v);
		got = vid_link_trace_read(dbase, sizeof(lost), gb, gc);
		CHECK(got == sizeof(lost));
		CHECK(gb[7] == 'D');
		CHECK(gc[7] == P);	// swallowed as a coordinate: the tell
		CHECK(gc[8] == S);	// and the stream resumes after it

		// Loopback bytes are never interpreted, and are marked so.
		v.write(STAT, VID_CTL_LOOPBACK);
		settle(v);
		vid_link_trace_span(&oldest, &next);
		v.write(DATA, 0x5A);
		v.tick(1);
		got = vid_link_trace_read(next, 1, gb, gc);
		CHECK(got == 1);
		CHECK(gb[0] == 0x5A);
		CHECK(gc[0] == VID_BYTE_LOOPBACK);
		(void)v.read(DATA);		// drain the echo
		v.write(STAT, VID_CTL_RESET);
		settle(v);

		// WRAP. After more than a ring's worth, the oldest offset still
		// held is exactly one ring behind, anything older is refused
		// rather than returned as stale data, and the newest is right.
		// $0E is an ignored control: cheap to send, still recorded.
		for (uint32_t i = 0; i < CMD_TRACE_ENTRIES + 100u; i++) {
			v.write(DATA, 0x0E);
			if ((i & 511u) == 511u)
				settle(v);
		}
		v.write(DATA, 'Q');
		settle(v);
		vid_link_trace_span(&oldest, &next);
		CHECK(next - oldest == CMD_TRACE_ENTRIES);
		CHECK(vid_link_trace_read(oldest - 1u, 1, gb, gc) == 0);
		CHECK(vid_link_trace_read(next - 1u, 1, gb, gc) == 1);
		CHECK(gb[0] == 'Q');
		CHECK(gc[0] == S);
		CHECK(vid_link_trace_read(next, 1, gb, gc) == 0); // not yet
	}

	// THE PATTERN RULE, driven through the renderer directly so each
	// property is isolated: the AND with a NON-white colour, the WINDOW
	// anchor, the wrap left of that anchor, and flood fill's two failure
	// modes (VID-033, VID-034).
	{
		static const uint8_t CHECKER[4] = { 0xFF, 0x00, 0x00, 0xFF };
		// A 3x2 tile whose every cell differs, so no shift of the
		// anchor can reproduce it -- the lesson of the diagonal weave.
		static const uint8_t ODD[6] = { 0xF0, 0x0F, 0xCC, 0x33, 0xAA,
		    0x55 };
		uint32_t over, flat;

		v.write(STAT, VID_CTL_RESET);
		settle(v);
		vidterm_set_logic(0);
		vidterm_set_origin(0, 0);
		vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
		vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x00);
		vidterm_buf_define(8, 1, 6);
		vidterm_buf_shape(8, 1, 3, 2);
		for (int i = 0; i < 6; i++)
			vidterm_buf_poke(8, 1, (uint32_t)i, ODD[i]);
		vidterm_set_pattern(8, 1);

		// AND, not replace: red (0xE0) through the tile.
		for (int x = 10; x < 16; x++)
			vidterm_plot(x, 10, 0xE0);
		int bad = 0;
		for (int x = 10; x < 16; x++)
			if (video_framebuffer[10][x] !=
			    (uint8_t)(0xE0 & ODD[(10 % 2) * 3 + (x % 3)]))
				bad++;
		CHECK(bad == 0);

		// WINDOW ANCHOR: with the origin at 100,51 the tile starts
		// there, not at the panel's corner. Both coordinates are
		// chosen out of phase with the 3x2 tile (100 mod 3 = 1, 51 mod
		// 2 = 1), so a screen-anchored tile disagrees in x AND y.
		vidterm_set_origin(100, 51);
		for (int x = 0; x < 6; x++) {
			vidterm_plot(x, 0, 0xFF);
			vidterm_plot(x, 1, 0xFF);
		}
		bad = 0;
		for (int y = 0; y < 2; y++)
			for (int x = 0; x < 6; x++)
				if (video_framebuffer[51 + y][100 + x] !=
				    ODD[y * 3 + (x % 3)])
					bad++;
		if (bad)
			fprintf(stderr, "  pattern anchor: %d pixels tiled from"
			    " the screen, not the window\n", bad);
		CHECK(bad == 0);

		// LEFT OF THE ORIGIN, as an unprotected window may draw: -1
		// must wrap to the tile's last column, not index before it.
		vidterm_plot(-1, 0, 0xFF);
		CHECK(video_framebuffer[51][99] == ODD[0 * 3 + 2]);

		// VID-034: FFill with a pattern that CONTAINS the target colour.
		// Before the two-pass fill this painted a fragment of the region
		// (6,144 of 491,520 on a full panel) and dropped seeds.
		vidterm_set_origin(0, 0);
		vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x00);
		vidterm_buf_define(8, 2, 4);
		vidterm_buf_shape(8, 2, 2, 2);
		for (int i = 0; i < 4; i++)
			vidterm_buf_poke(8, 2, (uint32_t)i, CHECKER[i]);
		vidterm_set_pattern(8, 2);
		uint32_t over0, flat0;
		vidterm_fill_stats(&over0, &flat0);
		vidterm_flood_fill(10, 10, 0xFF);
		bad = 0;
		for (int y = 0; y < FB_HEIGHT; y++)
			for (int x = 0; x < FB_WIDTH; x++)
				if (video_framebuffer[y][x] !=
				    CHECKER[(y % 2) * 2 + (x % 2)])
					bad++;
		if (bad)
			fprintf(stderr, "  patterned FFill: %d of %d pixels"
			    " wrong\n", bad, FB_WIDTH * FB_HEIGHT);
		CHECK(bad == 0);
		vidterm_fill_stats(&over, &flat);
		CHECK(over == over0);	// no seeds dropped
		CHECK(flat == flat0);	// every span patterned

		// VID-034 without a pattern: LSet AND. With colour 0x3F over a
		// 0x0F region, every filled pixel is 0x3F AND 0x0F = 0x0F -- the
		// TARGET -- so a one-pass fill re-seeds its own work and drops
		// seeds. The pixels look unchanged either way (that is what AND
		// gives), so the discriminating check is that no seed was
		// dropped; a first draft of this used 0x3C, whose AND differs
		// from the target, and would have passed on the broken fill.
		vidterm_set_pattern(0, 0);
		vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x00);
		vidterm_fill_rect(0, 0, 64, 64, 0x0F);	// a background to AND
		vidterm_fill_rect(0, 0, 64, 1, 0x00);	// ... fenced so the
		vidterm_fill_rect(0, 63, 64, 1, 0x00);	// fill stays in it:
		vidterm_set_logic(1);				// AND
		vidterm_fill_stats(&over0, &flat0);
		vidterm_flood_fill(10, 10, 0x3F);	// target 0x0F
		vidterm_set_logic(0);
		bad = 0;
		for (int y = 1; y < 63; y++)
			for (int x = 0; x < 64; x++)
				if (video_framebuffer[y][x] != 0x0Fu)
					bad++;
		CHECK(bad == 0);
		vidterm_fill_stats(&over, &flat);
		CHECK(over == over0);
		CHECK(flat == flat0);
		CHECK(video_framebuffer[10][64] == 0x00);	// did not leak
		vidterm_buf_kill(8, 1);
		vidterm_buf_kill(8, 2);
	}

	// THE SPAN WRITER AGAINST AN ORACLE (VID-035). Patterned and mixed
	// fills, hlines, flood fills and PutBlks now compose a span at a time
	// instead of a pixel at a time. The rule must not move by a single
	// pixel, so a MODEL plane applies it by its written definition --
	// colour AND pattern (window-anchored, per pixel), then the mix mode
	// against the graphics plane -- and the renderer must agree with it on
	// BOTH planes: the framebuffer directly, then the shadow, by refreshing
	// transparent spaces over everything so the screen shows the shadow.
	//
	// The cases are the ones the span code has to get right that the old
	// per-pixel code got right for free: every mix mode; spans crossing the
	// 64-byte chunk boundary at an odd start; a window origin with drawing
	// left of it; drawn (resident) and undrawn (implicit, non-zero uniform)
	// background rows in the same span of rows; a pattern in the SRAM cache,
	// one in PSRAM narrower than a chunk, and one wider; a clipped PutBlk
	// with a varying source; and a flood fill.
	{
		static uint8_t model[FB_HEIGHT][FB_WIDTH];
		static const uint8_t U = 0x49;		// implicit rows' colour
		static const uint8_t ODD[6] = { 0xF0, 0x0F, 0xCC, 0x33, 0xAA,
		    0x55 };
		int ox = 0, oy = 0, pw = 0, ph = 0, logic = 0;
		const uint8_t *pat = nullptr;
		static uint8_t pbuf[400];

		auto mix = [&](uint8_t b, uint8_t c) -> uint8_t {
			return logic == 1 ? (uint8_t)(b & c) :
			    logic == 2 ? (uint8_t)(b | c) :
			    logic == 3 ? (uint8_t)(b ^ c) : c;
		};
		auto wrap = [](int v, int m) { v %= m; return v < 0 ? v + m : v; };
		auto model_px = [&](int px, int py, uint8_t c) {
			if (pat != nullptr)
				c &= pat[wrap(py - oy, ph) * pw + wrap(px - ox, pw)];
			model[py][px] = mix(model[py][px], c);
		};
		auto set_pat = [&](uint8_t id, int w, int h,
		    uint8_t (*f)(int, int)) {
			vidterm_buf_define(9, id, (uint32_t)(w * h));
			vidterm_buf_shape(9, id, (uint16_t)w, (uint16_t)h);
			for (int i = 0; i < w * h; i++) {
				pbuf[i] = f(i % w, i / w);
				vidterm_buf_poke(9, id, (uint32_t)i, pbuf[i]);
			}
			vidterm_set_pattern(9, id);
			pat = pbuf; pw = w; ph = h;
		};
		auto background = [&]() {
			vidterm_set_pattern(0, 0);
			pat = nullptr;
			vidterm_set_logic(0); logic = 0;
			vidterm_set_origin(0, 0); ox = oy = 0;
			vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
			vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, U);
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					model[y][x] = U;
			// Rows 100..199 drawn (resident) with a gradient, so
			// a span of rows 90..210 crosses both kinds.
			for (int y = 100; y < 200; y++) {
				const uint8_t c = (uint8_t)(y * 7 + 3);
				vidterm_fill_rect(0, y, FB_WIDTH, 1, c);
				for (int x = 0; x < FB_WIDTH; x++)
					model[y][x] = c;
			}
		};
		auto compare = [&](const char *what) {
			int bad = 0, fx = -1, fy = -1;
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					if (video_framebuffer[y][x] != model[y][x]) {
						if (!bad) { fx = x; fy = y; }
						bad++;
					}
			if (bad)
				fprintf(stderr, "  span oracle, %s: %d px differ,"
				    " first %d,%d (got %02X want %02X)\n", what,
				    bad, fx, fy, video_framebuffer[fy][fx],
				    model[fy][fx]);
			CHECK(bad == 0);
		};
		auto compare_both = [&](const char *what) {
			compare(what);
			// Transparent spaces everywhere: every cell restores
			// the graphics plane, so the screen IS the shadow.
			vidterm_set_pattern(0, 0);
			(void)vidterm_clear(' ', 0x01u);
			vidterm_refresh();
			char w2[80];
			snprintf(w2, sizeof(w2), "%s (shadow)", what);
			compare(w2);
		};

		v.write(STAT, VID_CTL_RESET);
		settle(v);

		// A: small cached pattern, XOR, window at 100,51, a rectangle
		// starting LEFT of the origin and 150 wide (crosses two chunk
		// boundaries), over implicit and resident rows.
		background();
		set_pat(1, 3, 2, [](int x, int y) { return ODD[y * 3 + x]; });
		vidterm_set_origin(100, 51); ox = 100; oy = 51;
		// An OPAQUE glyph inside the target area first, so the
		// framebuffer there differs from the graphics plane: a mix that
		// read the framebuffer instead of the plane would combine with
		// the glyph and disagree with the model. Without this the two
		// are identical and that mistake is invisible.
		vidterm_putch(10, 6, 'M', 0xE0u);
		vidterm_refresh();
		vidterm_set_logic(3); logic = 3;
		// Starts 62 LEFT and 10 ABOVE the origin. Both offsets are
		// chosen so the wrap is really exercised: a first version began
		// 63 to the left, a multiple of the tile's width of 3, so the
		// negative remainder never occurred and a missing wrap passed.
		vidterm_fill_rect(-62, -10, 150, 171, 0xE7);
		for (int y = 51 - 10; y < 51 - 10 + 171; y++)
			for (int x = 100 - 62; x < 100 - 62 + 150; x++)
				model_px(x, y, 0xE7);
		compare_both("A cached pattern, XOR");

		// B: a pattern too big for the SRAM cache but narrower than a
		// chunk (20x16 = 320 B, read a row at a time), AND.
		background();
		set_pat(2, 20, 16, [](int x, int y) {
			return (uint8_t)(x * 13 + y * 29 + 7); });
		vidterm_set_logic(1); logic = 1;
		vidterm_fill_rect(5, 95, 301, 20, 0xDB);
		for (int y = 95; y < 115; y++)
			for (int x = 5; x < 306; x++)
				model_px(x, y, 0xDB);
		compare_both("B PSRAM pattern < chunk, AND");

		// C: a pattern wider than a chunk (100x3 = 300 B), OR.
		background();
		set_pat(3, 100, 3, [](int x, int y) {
			return (uint8_t)(x * 5 + y * 71 + 1); });
		vidterm_set_logic(2); logic = 2;
		vidterm_fill_rect(1, 150, 257, 60, 0x1C);
		for (int y = 150; y < 210; y++)
			for (int x = 1; x < 258; x++)
				model_px(x, y, 0x1C);
		compare_both("C PSRAM pattern > chunk, OR");

		// D: a PutBlk with a varying source, patterned and XORed,
		// through a window, CLIPPED on the right.
		background();
		set_pat(4, 3, 2, [](int x, int y) { return ODD[y * 3 + x]; });
		vidterm_set_origin(100, 51); ox = 100; oy = 51;
		vidterm_set_clip(0, 0, 170, FB_HEIGHT);
		vidterm_set_logic(3); logic = 3;
		vidterm_buf_define(9, 7, 90 * 5);
		vidterm_buf_shape(9, 7, 90, 5);
		uint8_t srcb[90 * 5];
		for (int i = 0; i < 90 * 5; i++) {
			srcb[i] = (uint8_t)(i * 3 + (i / 90) * 17);
			vidterm_buf_poke(9, 7, (uint32_t)i, srcb[i]);
		}
		vidterm_buf_put(9, 7, 5, 44);	// abs 105,95; clipped at 170
		for (int r = 0; r < 5; r++)
			for (int c = 0; c < 90; c++) {
				const int x = 105 + c, y = 95 + r;
				if (x < 170)
					model_px(x, y, srcb[r * 90 + c]);
			}
		vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
		compare_both("D clipped PutBlk, pattern, XOR");

		// F: a PARTIAL span starting at x = 0 over UNDRAWN rows. The
		// full-row shortcut (mark the shadow row real without filling
		// it, since the span overwrites it all) must not fire here: the
		// rest of each row has to become the uniform colour, and if it
		// did fire, whatever the shadow array last held would show.
		background();
		set_pat(6, 3, 2, [](int x, int y) { return ODD[y * 3 + x]; });
		vidterm_fill_rect(0, 20, 40, 3, 0xFF);
		for (int y = 20; y < 23; y++)
			for (int x = 0; x < 40; x++)
				model_px(x, y, 0xFF);
		compare_both("F partial span at x=0, undrawn rows");

		// G: the same partial span at x = 0, PLAIN (no pattern, no mix),
		// which goes through the ordinary fill path rather than the span
		// writer. It gained the same full-row shortcut, so it needs the
		// same proof that the shortcut does not fire on a partial row.
		background();
		vidterm_fill_rect(0, 20, 40, 3, 0x6B);
		for (int y = 20; y < 23; y++)
			for (int x = 0; x < 40; x++)
				model_px(x, y, 0x6B);
		compare_both("G plain partial span at x=0, undrawn rows");

		// E: a patterned flood fill with OR, in a fenced rectangle of
		// the implicit colour, so the region is known exactly.
		background();
		vidterm_fill_rect(300, 250, 200, 1, 0x00);
		vidterm_fill_rect(300, 330, 200, 1, 0x00);
		vidterm_fill_rect(300, 250, 1, 81, 0x00);
		vidterm_fill_rect(499, 250, 1, 81, 0x00);
		for (int x = 300; x < 500; x++) model[250][x] = model[330][x] = 0;
		for (int y = 250; y < 331; y++) model[y][300] = model[y][499] = 0;
		set_pat(5, 3, 2, [](int x, int y) { return ODD[y * 3 + x]; });
		vidterm_set_logic(2); logic = 2;
		vidterm_flood_fill(400, 290, 0x92);
		for (int y = 251; y < 330; y++)
			for (int x = 301; x < 499; x++)
				model_px(x, y, 0x92);
		compare_both("E patterned OR flood fill");

		vidterm_set_logic(0);
		vidterm_set_pattern(0, 0);
		vidterm_set_origin(0, 0);
		for (uint8_t id = 1; id <= 7; id++)
			vidterm_buf_kill(9, id);
	}

	// A PATTERNED SPAN WITH NO MIX MODE -- the patterned Bar, the case wgfx
	// draws -- is built as one period of the pattern row and then copied
	// along the row, rather than composed pixel by pixel. So the period, its
	// phase at the span's start, the colour AND, and the copy's tail all
	// have to be right at every alignment: pattern widths either side of a
	// word and of the copy's own 64-byte chunk, ones in the SRAM cache and
	// in PSRAM, spans shorter than a period, a few periods, and the panel,
	// starting at each byte of a word. A mix mode with no pattern, which
	// takes the same writer, is checked over drawn and undrawn rows. Both
	// planes are compared, the shadow through transparent spaces.
	{
		static uint8_t model[FB_HEIGHT][FB_WIDTH];
		static const uint8_t U = 0x2Du;
		static uint8_t pbuf[1200];
		auto compare = [&](const char *what) {
			for (int pass = 0; pass < 2; pass++) {
				if (pass == 1) {
					vidterm_set_pattern(0, 0);
					(void)vidterm_clear(' ', 0x01u);
					vidterm_refresh();
				}
				int bad = 0, fx = -1, fy = -1;
				for (int y = 0; y < FB_HEIGHT; y++)
					for (int x = 0; x < FB_WIDTH; x++)
						if (video_framebuffer[y][x] !=
						    model[y][x]) {
							if (!bad) { fx = x; fy = y; }
							bad++;
						}
				if (bad)
					fprintf(stderr, "  period oracle, %s%s: %d"
					    " px differ, first %d,%d (got %02X"
					    " want %02X)\n", what, pass ?
					    " (shadow)" : "", bad, fx, fy,
					    video_framebuffer[fy][fx],
					    model[fy][fx]);
				CHECK(bad == 0);
			}
		};
		auto background = [&]() {
			v.write(STAT, VID_CTL_RESET);
			settle(v);
			vidterm_set_pattern(0, 0);
			vidterm_set_logic(0);
			vidterm_set_origin(0, 0);
			vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
			vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, U);
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					model[y][x] = U;
		};
		static const struct { int w, h; } SHAPES[] = {
			{ 1, 1 }, { 3, 2 }, { 4, 1 }, { 5, 3 }, { 7, 2 },
			{ 16, 16 }, { 20, 4 }, { 64, 2 }, { 65, 1 },
			{ 100, 3 }, { 300, 1 }, { 1024, 1 },
		};
		static const int LENS[] = { 1, 2, 5, 63, 64, 129, 333, 1017 };
		uint8_t id = 1;

		for (const auto &s : SHAPES) {
			background();
			const int pw = s.w, ph = s.h;
			vidterm_buf_define(10, id, (uint32_t)(pw * ph));
			vidterm_buf_shape(10, id, (uint16_t)pw, (uint16_t)ph);
			for (int i = 0; i < pw * ph; i++) {
				pbuf[i] = (uint8_t)(i * 29 + (i / pw) * 83 + 5);
				vidterm_buf_poke(10, id, (uint32_t)i, pbuf[i]);
			}
			vidterm_set_pattern(10, id);
			// An origin off the byte grid, so the phase is never
			// trivially zero.
			const int ox = 7, oy = 3;
			vidterm_set_origin(ox, oy);

			int y = 0;
			for (int start = 0; start < 5; start++)
				for (int li = 0; li < 8; li++, y += 2) {
					const int len = LENS[li] - start;
					const uint8_t col = (li & 1) ? 0xFF : 0xDB;

					if (len <= 0)
						continue;
					vidterm_fill_rect(start - ox, y, len, 2, col);
					for (int yy = oy + y; yy < oy + y + 2; yy++)
						for (int x = start; x < start + len;
						    x++) {
							int tx = (x - ox) % pw, ty =
							    (yy - oy) % ph;
							if (tx < 0) tx += pw;
							model[yy][x] = (uint8_t)(col &
							    pbuf[ty * pw + tx]);
						}
				}
			// And the whole panel, which is a different route in
			// fill_rect for a plain colour and must not be one here.
			vidterm_set_origin(0, 0);
			vidterm_fill_rect(0, 200, FB_WIDTH, FB_HEIGHT - 200, 0xB6);
			for (int yy = 200; yy < FB_HEIGHT; yy++)
				for (int x = 0; x < FB_WIDTH; x++)
					model[yy][x] = (uint8_t)(0xB6 &
					    pbuf[(yy % ph) * pw + x % pw]);
			char what[40];
			snprintf(what, sizeof(what), "pattern %dx%d", pw, ph);
			compare(what);
			vidterm_set_pattern(0, 0);
			vidterm_buf_kill(10, id);
			id++;
		}

		// XOR with no pattern, over rows 40..99 drawn with a gradient
		// that varies along the row and rows 100..139 left undrawn.
		background();
		for (int y = 40; y < 100; y++)
			for (int x = 0; x < FB_WIDTH; x++) {
				const uint8_t c = (uint8_t)(x * 13 + y * 7);
				vidterm_plot(x, y, c);
				model[y][x] = c;
			}
		vidterm_set_logic(3);
		vidterm_fill_rect(3, 30, 1017, 110, 0x5A);
		for (int y = 30; y < 140; y++)
			for (int x = 3; x < 1020; x++)
				model[y][x] ^= 0x5A;
		vidterm_set_logic(0);
		compare("XOR, no pattern");
	}

	// FLOOD FILL AGAINST TWO ORACLES, over a busy picture. The fill searches
	// a word at a time and stacks parent segments, scanning back towards
	// the row it came from only where a run leaks past the parent -- so
	// its seeds and their order are its own. What must not change is the
	// RESULT. The first oracle is the plain byte-wise seed-point scanline
	// fill, run over a copy of the screen; the second is an independent
	// 4-connected flood. Both planes must match the first, the second must
	// agree the region is complete, and nothing may be dropped. The
	// boundary is thousands of short random walls, so runs begin and end
	// at every byte of a word and leak past their parents constantly, and
	// the clip is off the word grid on both sides.
	{
		static uint8_t ref[FB_HEIGHT][FB_WIDTH];
		static const uint8_t U = 0x31u, W = 0xC4u, C = 0x7Eu;
		const int cx0 = 13, cy0 = 7, cx1 = 13 + 990, cy1 = 7 + 460;

		v.write(STAT, VID_CTL_RESET);
		settle(v);
		vidterm_set_pattern(0, 0);
		vidterm_set_logic(0);
		vidterm_set_origin(0, 0);
		vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
		vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, U);
		uint32_t seed = 12345u;
		auto rnd = [&seed](uint32_t n) {
			seed = seed * 1103515245u + 12345u;
			return (seed >> 8) % n;
		};
		for (int i = 0; i < 9000; i++) {
			const int x = (int)rnd(FB_WIDTH), y = (int)rnd(FB_HEIGHT);
			if (i & 1)
				vidterm_fill_rect(x, y, 1 + (int)rnd(9), 1, W);
			else
				vidterm_fill_rect(x, y, 1, 1 + (int)rnd(6), W);
		}
		vidterm_fill_rect(500, 240, 1, 1, U);	// the seed is target
		memcpy(ref, video_framebuffer, sizeof(ref));

		// The byte-wise fill, exactly as it was.
		unsigned ref_overflows = 0;
		{
			static struct { int16_t x, y; } st[FILL_SEEDS];
			unsigned sp = 0;
			const uint8_t target = ref[240][500];
			st[sp].x = 500; st[sp].y = 240; sp++;
			while (sp > 0u) {
				sp--;
				const int py = st[sp].y, px = st[sp].x;
				if (ref[py][px] != target)
					continue;
				int x1 = px, x2 = px;
				while (x1 > cx0 && ref[py][x1 - 1] == target)
					x1--;
				while (x2 < cx1 - 1 && ref[py][x2 + 1] == target)
					x2++;
				memset(&ref[py][x1], C, (size_t)(x2 + 1 - x1));
				for (int ny = py - 1; ny <= py + 1; ny += 2) {
					if (ny < cy0 || ny >= cy1)
						continue;
					int i = x1;
					while (i <= x2) {
						if (ref[ny][i] != target) {
							i++;
							continue;
						}
						if (sp < FILL_SEEDS) {
							st[sp].x = (int16_t)i;
							st[sp].y = (int16_t)ny;
							sp++;
						} else {
							ref_overflows++;
						}
						while (i <= x2 && ref[ny][i] == target)
							i++;
					}
				}
			}
		}

		uint32_t ov0, un0, ov1, un1;
		vidterm_fill_stats(&ov0, &un0);
		vidterm_set_clip(cx0, cy0, cx1 - cx0, cy1 - cy0);
		vidterm_flood_fill(500, 240, C);
		vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);
		vidterm_fill_stats(&ov1, &un1);

		int filled = 0;
		for (int y = 0; y < FB_HEIGHT; y++)
			for (int x = 0; x < FB_WIDTH; x++)
				filled += ref[y][x] == C;
		CHECK(filled > 20000);		// the region is not trivial
		CHECK(ov1 - ov0 == ref_overflows);
		// The stack is sized so an ordinary picture cannot overflow it;
		// this one dropped 835 seeds against the old 96-entry stack.
		CHECK(ref_overflows == 0u);

		// And the region is COMPLETE: an independent 4-connected flood
		// over the same picture reaches exactly the pixels filled. The
		// oracle above only says the fill matches an algorithm; this
		// says the algorithm reached everything it should.
		{
			static uint8_t reach[FB_HEIGHT][FB_WIDTH];
			static int16_t q[FB_HEIGHT * FB_WIDTH][2];
			memset(reach, 0, sizeof(reach));
			int qh = 0, qt = 0, bfs_bad = 0;
			q[qt][0] = 500; q[qt][1] = 240; qt++;
			reach[240][500] = 1;
			while (qh < qt) {
				const int x = q[qh][0], y = q[qh][1];
				qh++;
				static const int D[4][2] = { { 1, 0 }, { -1, 0 },
				    { 0, 1 }, { 0, -1 } };
				for (const auto &d : D) {
					const int nx = x + d[0], ny = y + d[1];
					if (nx < cx0 || nx >= cx1 || ny < cy0 ||
					    ny >= cy1 || reach[ny][nx] ||
					    video_framebuffer[ny][nx] == W)
						continue;
					// Unreached target pixels are U; filled
					// ones are C. Walls stop the flood.
					reach[ny][nx] = 1;
					q[qt][0] = (int16_t)nx;
					q[qt][1] = (int16_t)ny;
					qt++;
				}
			}
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					if ((reach[y][x] != 0) !=
					    (video_framebuffer[y][x] == C))
						bfs_bad++;
			if (bfs_bad)
				fprintf(stderr, "  flood oracle: %d px differ"
				    " from a 4-connected flood\n", bfs_bad);
			CHECK(bfs_bad == 0);
		}
		for (int pass = 0; pass < 2; pass++) {
			if (pass == 1) {
				(void)vidterm_clear(' ', 0x01u);
				vidterm_refresh();
			}
			int bad = 0, fx = -1, fy = -1;
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					if (video_framebuffer[y][x] != ref[y][x]) {
						if (!bad) { fx = x; fy = y; }
						bad++;
					}
			if (bad)
				fprintf(stderr, "  flood oracle%s: %d px differ,"
				    " first %d,%d (got %02X want %02X)\n",
				    pass ? " (shadow)" : "", bad, fx, fy,
				    video_framebuffer[fy][fx], ref[fy][fx]);
			CHECK(bad == 0);
		}
		fprintf(stderr, "  flood oracle: %d px filled, %u seeds"
		    " dropped\n", filled, ref_overflows);
	}

	// A FLOOD FILL FROM THE GUEST IS SPREAD OVER RENDER PASSES, AND NOTHING
	// DRAWS UNDER IT. A full-panel fill costs more than a frame, so the
	// console starts it and each pass does a slice. The fill finds its way
	// by the colours on screen, so a command after it in the SAME burst
	// must not run until it is done: here a full-width Bar across the
	// panel follows the FFill. Run in order, the fill covers everything and
	// the Bar lands on top. If the Bar ran mid-fill it would be a wall, and
	// everything beyond it would be left unfilled -- so the check is simply
	// that no pixel of the old colour survives, and that it really took
	// more than one pass, or the hold was never exercised.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);
		const uint8_t before = video_framebuffer[10][10];

		v.write(DATA, ESC); v.write(DATA, 0x40);	// SetDPtr 10,10
		v.write(DATA, 0); v.write(DATA, 10);
		v.write(DATA, 0); v.write(DATA, 10);
		v.write(DATA, ESC); v.write(DATA, 0x4F);	// FFill
		v.write(DATA, ESC); v.write(DATA, 0x40);	// SetDPtr 0,300
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, 1); v.write(DATA, 0x2C);
		v.write(DATA, ESC); v.write(DATA, 0x4A);	// Bar to 1023,301
		v.write(DATA, 3); v.write(DATA, 0xFF);
		v.write(DATA, 1); v.write(DATA, 0x2D);

		int passes = 0;
		bool held = false;
		while (!vid_link_idle() && passes < 256) {
			v.tick(1);
			passes++;
			// Mid-fill, the Bar's bytes must still be queued.
			if (vidterm_fill_pending())
				held = held || !vid_link_idle();
		}
		// The cursor and pointer are drawn INTO the framebuffer, and
		// both can show the old colour (an inverted white cell is
		// black; so is the pointer's outline), so they come off first.
		vidterm_cursor_hide();
		int left = 0;
		for (int y = 0; y < FB_HEIGHT; y++)
			for (int x = 0; x < FB_WIDTH; x++)
				left += video_framebuffer[y][x] == before;
		if (left != 0 || passes < 3) {
			int bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					if (video_framebuffer[y][x] == before) {
						if (x < bx0) bx0 = x;
						if (y < by0) by0 = y;
						if (x > bx1) bx1 = x;
						if (y > by1) by1 = y;
					}
			fprintf(stderr, "  sliced FFill: %d px of the old colour"
			    " left (%d,%d)-(%d,%d), %d passes\n", left, bx0,
			    by0, bx1, by1, passes);
		}
		CHECK(passes >= 3);		// it really was sliced
		CHECK(held);
		CHECK(left == 0);		// the Bar did not wall it off
		CHECK(!vidterm_fill_pending());

		// A link reset mid-fill drops the fill: the guest asked for a
		// clean slate, and a fill still running afterwards would draw
		// over whatever it does next.
		v.write(DATA, 0x0C);
		settle(v);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 10);
		v.write(DATA, 0); v.write(DATA, 10);
		v.write(DATA, ESC); v.write(DATA, 0x4F);
		v.tick(1);
		CHECK(vidterm_fill_pending());	// started, not finished
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		CHECK(!vidterm_fill_pending());
		settle(v);
		CHECK(vid_link_idle());
	}

	// WITH NO PSRAM. Every PSRAM-backed store returns NULL when the probe
	// finds no part, and every caller has a fallback -- all of it shipped in
	// every image, none of it ever run until this: the shims always handed
	// out arrays and the board always has its part. The probe has already
	// been wrong on this board once (it reported 8 MB with chip select never
	// assigned), so a probe that one day correctly says "none" must land on
	// paths that have been exercised. Each check states the degradation the
	// design promises, and the switch is put back before anything else runs.
	{
		static const uint8_t ESC = 0x1B;
		pv_psram_set_absent(true);
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);
		vidterm_set_origin(0, 0);
		vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);

		// The switch is the firmware's one verdict, seen everywhere.
		CHECK(win_store_slots() == 0u);
		CHECK(gfx_shadow_base() == nullptr);
		CHECK(gfx_bufpool_base() == nullptr);
		CHECK(cmd_trace_base() == nullptr);
		CHECK(fill_log_base() == nullptr);
		CHECK(fill_seeds_base() == nullptr);

		// Transparent text has nothing to restore, so it paints its
		// background -- text hides graphics, as it did before the
		// shadow existed -- and erasing a glyph leaves no strokes
		// behind. Ghosting would be the failure: a transparent cell
		// that neither restores nor paints.
		{
			const int col = 20, row = 6;
			const int x = col * CHAR_W, y = row * CHAR_H;
			// A panel-wide fill first, so the plane's uniform
			// colour is 0x49 -- neither the transparent background
			// (0x00) nor the bar. Cleared to 0x00, a blit that fell
			// back to the uniform colour would be indistinguishable
			// from one that painted the background, and did pass.
			vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x49);
			vidterm_fill_rect(x - 8, y - 8, 40, 40, 0x6D);
			vidterm_putch(col, row, 'M', 0xE1);	// white, transparent
			vidterm_refresh();
			int fg = 0, bg = 0, other = 0;
			for (int gy = 0; gy < CHAR_H; gy++)
				for (int gx = 0; gx < CHAR_W; gx++) {
					const uint8_t p = video_framebuffer[y + gy][x + gx];
					fg += p == 0xFF;
					bg += p == 0x00;
					other += p != 0xFF && p != 0x00;
				}
			CHECK(fg > 10 && bg > 10 && other == 0);
			vidterm_putch(col, row, ' ', 0xE1);
			vidterm_refresh();
			int left = 0;
			for (int gy = 0; gy < CHAR_H; gy++)
				for (int gx = 0; gx < CHAR_W; gx++)
					left += video_framebuffer[y + gy][x + gx] != 0x00;
			CHECK(left == 0);
		}

		// A MIX MODE combines with what is on screen. With no graphics
		// plane to consult, the framebuffer is the best account of the
		// picture there is; combining with a flat colour instead would
		// replace a drawn picture with a flat one. Both the span path
		// (a fill) and the single-pixel path (a plot).
		{
			auto pic = [](int x, int y) {
				return (uint8_t)(x * 7 + y * 13 + 1);
			};
			for (int y = 300; y < 320; y++)
				for (int x = 100; x < 230; x++)
					vidterm_plot(x, y, pic(x, y));
			vidterm_set_logic(3);
			vidterm_fill_rect(100, 300, 130, 10, 0x5A);	// span
			for (int x = 100; x < 230; x++)
				vidterm_plot(x, 315, 0xA5);		// pixel
			vidterm_set_logic(0);
			int bad = 0;
			for (int y = 300; y < 320; y++)
				for (int x = 100; x < 230; x++) {
					uint8_t want = pic(x, y);
					if (y < 310) want ^= 0x5A;
					if (y == 315) want ^= 0xA5;
					bad += video_framebuffer[y][x] != want;
				}
			if (bad)
				fprintf(stderr, "  no PSRAM: %d mixed px wrong\n", bad);
			CHECK(bad == 0);
		}

		// A FLOOD FILL has a 64-entry stack on its own frame, cannot be
		// suspended, and so finishes inside the command: no hold, no
		// job left pending. On a busy picture that small stack binds,
		// and the dropped work is COUNTED; whatever is filled must lie
		// inside the true region, never outside it.
		{
			static uint8_t pic0[FB_HEIGHT][FB_WIDTH];
			vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x31);
			uint32_t seed = 777u;
			auto rnd = [&seed](uint32_t n) {
				seed = seed * 1103515245u + 12345u;
				return (seed >> 8) % n;
			};
			for (int i = 0; i < 6000; i++)
				vidterm_fill_rect((int)rnd(FB_WIDTH),
				    (int)rnd(FB_HEIGHT), 1 + (int)rnd(9), 1, 0xC4);
			vidterm_fill_rect(500, 240, 1, 1, 0x31);
			memcpy(pic0, video_framebuffer, sizeof(pic0));
			uint32_t ov0, un0, ov1, un1;
			vidterm_fill_stats(&ov0, &un0);
			CHECK(!vidterm_flood_fill_start(500, 240, 0x7E));
			CHECK(!vidterm_fill_pending());
			vidterm_fill_stats(&ov1, &un1);
			CHECK(ov1 > ov0);		// the small stack bound, and said so
			int outside = 0, filled = 0;
			for (int y = 0; y < FB_HEIGHT; y++)
				for (int x = 0; x < FB_WIDTH; x++)
					if (video_framebuffer[y][x] == 0x7E) {
						filled++;
						outside += pic0[y][x] != 0x31;
					}
			CHECK(filled > 1000 && outside == 0);

			// A two-pass fill (a mix mode) has no span log, so its
			// spans stay SOLID -- visibly unpatterned, consistent,
			// and counted -- rather than being lost.
			vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x31);
			vidterm_fill_rect(300, 200, 50, 1, 0xC4);
			vidterm_fill_rect(300, 240, 50, 1, 0xC4);
			vidterm_fill_rect(300, 200, 1, 41, 0xC4);
			vidterm_fill_rect(349, 200, 1, 41, 0xC4);
			vidterm_set_logic(3);
			vidterm_fill_stats(&ov0, &un0);
			vidterm_flood_fill(320, 220, 0x7E);
			vidterm_fill_stats(&ov1, &un1);
			vidterm_set_logic(0);
			CHECK(un1 - un0 == 39u);	// one per row of the box
			int solid = 0;
			for (int y = 201; y < 240; y++)
				for (int x = 301; x < 349; x++)
					solid += video_framebuffer[y][x] == 0x7E;
			CHECK(solid == 39 * 48);
			CHECK(video_framebuffer[199][320] == 0x31);	// contained
		}

		// A guest FFill through the link, on that same picture: it
		// completes in the pass that consumed it, so there is nothing
		// to hold and the link is idle straight after.
		{
			vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x31);
			v.write(DATA, ESC); v.write(DATA, 0x40);
			v.write(DATA, 0); v.write(DATA, 10);
			v.write(DATA, 0); v.write(DATA, 10);
			v.write(DATA, ESC); v.write(DATA, 0x4F);
			v.tick(1);
			CHECK(!vidterm_fill_pending());
			CHECK(video_framebuffer[FB_HEIGHT - 1][FB_WIDTH - 1] != 0x31);
		}

		// GET/PUT BUFFERS have no pool. A guest may still define one --
		// the descriptor is SRAM -- but nothing can be stored in it, so
		// a GetBlk captures nothing and a PutBlk draws nothing, rather
		// than stamping whatever a null pointer happens to reach.
		{
			vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x00);
			vidterm_fill_rect(10, 10, 16, 16, 0x6D);
			vidterm_buf_define(11, 1, 16u * 16u);
			CHECK(!vidterm_buf_poke(11, 1, 0, 0xFF));
			vidterm_buf_get(11, 1, 10, 10, 16, 16);
			vidterm_buf_put(11, 1, 200, 200);
			int drawn = 0;
			for (int y = 200; y < 216; y++)
				for (int x = 200; x < 216; x++)
					drawn += video_framebuffer[y][x] != 0x00;
			CHECK(drawn == 0);
			// And a pattern or a font taken from such a buffer is
			// refused: the plain colour and the built-in face stay.
			vidterm_set_pattern(11, 1);
			vidterm_fill_rect(300, 300, 8, 8, 0x5A);
			CHECK(video_framebuffer[300][300] == 0x5A);
			CHECK(video_framebuffer[307][307] == 0x5A);
			vidterm_buf_kill(11, 1);
		}

		// PARKED WINDOWS have no store. A window not on display has
		// nowhere to keep its text, so output to it is dropped -- NOT
		// written onto the window that is showing -- and showing it
		// later shows it empty, not whatever the previous window left
		// in that rectangle.
		{
			v.write(STAT, VID_CTL_RESET);
			settle(v);
			v.write(DATA, 0x0C);
			settle(v);
			v.write(DATA, 0x02); v.write(DATA, 4 + 0x20);
			v.write(DATA, 2 + 0x20); v.write(DATA, 'Z');	// root text
			v.write(DATA, ESC); v.write(DATA, 0x20);
			const uint8_t g[8] = { 1, 0, 4, 2, 10, 3, 0, 0 };
			for (uint8_t i = 0; i < 8; i++) v.write(DATA, g[i]);
			v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
			v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 1);
			v.write(DATA, 'Q');
			settle(v);
			CHECK(check_cell(4, 2, 'Z'));	// the root is untouched
			CHECK(!check_cell(4, 2, 'Q'));
			v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
			settle(v);
			CHECK(!check_cell(4, 2, 'Q'));	// dropped, as documented
			CHECK(check_cell(4, 2, ' '));	// shown EMPTY, not stale
			v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
			v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
			settle(v);
		}

		// THE TRACE has nowhere to live, and says so rather than
		// reporting an empty history as if nothing had been sent.
		{
			uint32_t oldest, next;
			vid_link_trace_span(&oldest, &next);
			CHECK(oldest == next);
			uint8_t b[4], c[4];
			CHECK(vid_link_trace_read(0, 4, b, c) == 0u);
		}

		pv_psram_set_absent(false);
		v.write(STAT, VID_CTL_RESET);
		settle(v);
		v.write(DATA, 0x0C);
		settle(v);
		CHECK(gfx_shadow_base() != nullptr);
	}

	// M0 loopback still works after M1 (the vlink test must keep passing).
	v.write(STAT, VID_CTL_LOOPBACK);
	v.write(DATA, 0x55);
	settle(v);
	CHECK(v.read(STAT) & VID_STAT_RDRF);
	CHECK(v.read(DATA) == 0x55);

	if (failures == 0) {
		printf("OK\n");
		return 0;
	}
	fprintf(stderr, "%d failure(s)\n", failures);
	return 1;
}
