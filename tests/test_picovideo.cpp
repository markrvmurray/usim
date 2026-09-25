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
	v.tick(1);
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));

	// Loopback on: bytes round-trip, in order, popped by Data reads.
	v.write(STAT, VID_CTL_LOOPBACK);
	v.write(DATA, 'X');
	v.write(DATA, 'Y');
	v.tick(1);
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
	v.tick(1);
	CHECK(v.read(DATA) == 'A');
	CHECK(v.read(DATA) == 'B');

	// Reserved registers are scratch: written value reads back.
	v.write(5, 0x5A);
	CHECK(v.read(5) == 0x5A);
	// ...and a Data write must NOT clobber the staged input byte.
	v.write(DATA, 'Z');
	v.tick(1);
	CHECK(v.read(STAT) & VID_STAT_RDRF);
	v.write(DATA, 0x00);	// enqueue another command byte
	CHECK(v.read(DATA) == 'Z');

	// Soft reset flushes both FIFOs (loopback bit carried in the same
	// write survives), and READY/version restage.
	v.write(DATA, 'Q');
	v.write(STAT, VID_CTL_RESET | VID_CTL_LOOPBACK);
	v.tick(1);
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
	v.tick(1);
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
	v.tick(1);
	CHECK(check_cell(0, 0, 'H'));
	CHECK(check_cell(1, 0, 'i'));
	CHECK(video_framebuffer[300][700] == 0x00);	// demo gone

	// CR returns to column 0 without a line feed: overwrite in place.
	v.write(DATA, 0x0D);
	v.write(DATA, 'X');
	v.tick(1);
	CHECK(check_cell(0, 0, 'X'));

	// LF moves down, keeping the column.
	v.write(DATA, 0x0A);
	v.write(DATA, 'Y');
	v.tick(1);
	CHECK(check_cell(1, 1, 'Y'));

	// Console bytes must not appear on the input FIFO when loopback is
	// off (they went to the renderer instead).
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));

	// Scrolling: line-feed to the bottom and once more; everything moves
	// up one row, so Y (row 1) lands on row 0 and X scrolls with it.
	for (int i = 0; i < 28; i++)
		v.write(DATA, 0x0A);	// row 1 + 28 = the bottom row
	v.tick(1);
	CHECK(check_cell(1, 1, 'Y'));	// not yet scrolled
	v.write(DATA, 0x0A);		// one beyond the bottom: scroll
	v.tick(1);
	CHECK(check_cell(1, 0, 'Y'));
	CHECK(!check_cell(0, 0, 'X'));	// X's row scrolled away... to row -1
	CHECK(check_cell(0, -1 + 1, 'X') == false);

	// ---- M2: the control-code table ----

	// Clear/home ($0C): wipes the text, next glyph lands at the origin.
	v.write(DATA, 0x0C);
	v.write(DATA, 'A');
	v.tick(1);
	CHECK(check_cell(0, 0, 'A'));
	CHECK(check_cell(1, 0, ' '));	// scrolled content gone

	// Cursor-XY ($02 col+$20 row+$20).
	v.write(DATA, 0x02);
	v.write(DATA, 0x20 + 10);
	v.write(DATA, 0x20 + 5);
	v.write(DATA, 'Q');
	v.tick(1);
	CHECK(check_cell(10, 5, 'Q'));

	// Home ($01).
	v.write(DATA, 0x01);
	v.write(DATA, 'H');
	v.tick(1);
	CHECK(check_cell(0, 0, 'H'));

	// Backspace ($08) is non-destructive cursor-left: "AB" BS 'C' -> "AC".
	v.write(DATA, 0x0C);
	v.write(DATA, 'A');
	v.write(DATA, 'B');
	v.write(DATA, 0x08);
	v.write(DATA, 'C');
	v.tick(1);
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
	v.tick(1);
	CHECK(check_cell(1, 0, 'B'));
	CHECK(check_cell(2, 0, ' '));
	CHECK(check_cell(5, 0, ' '));

	// Erase line ($03): whole row blank, column preserved.
	v.write(DATA, 0x03);
	v.write(DATA, 'Z');
	v.tick(1);
	CHECK(check_cell(0, 0, ' '));	// 'A' erased too
	CHECK(check_cell(2, 0, 'Z'));	// column was preserved

	// Cursor right ($06) skips a cell; cursor up ($09) climbs a row.
	v.write(DATA, 0x0C);
	v.write(DATA, 'A');
	v.write(DATA, 0x06);
	v.write(DATA, 'B');
	v.tick(1);
	CHECK(check_cell(0, 0, 'A'));
	CHECK(check_cell(1, 0, ' '));
	CHECK(check_cell(2, 0, 'B'));
	v.write(DATA, 0x02);
	v.write(DATA, 0x20 + 3);
	v.write(DATA, 0x20 + 4);
	v.write(DATA, 0x09);
	v.write(DATA, 'U');
	v.tick(1);
	CHECK(check_cell(3, 3, 'U'));

	// Cursor on/off ($05 $21/$20): argument consumed, not printed.
	v.write(DATA, 0x0C);
	v.write(DATA, 'A');
	v.write(DATA, 0x05);
	v.write(DATA, 0x20);
	v.write(DATA, 'B');
	v.tick(1);
	CHECK(check_cell(1, 0, 'B'));

	// Bell ($07) is a no-op on this device.
	v.write(DATA, 0x07);
	v.write(DATA, 'C');
	v.tick(1);
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
	v.tick(1);
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
	v.tick(1);
	CHECK(video_framebuffer[100][200] == 0xFF);

	// Palette + FColor: register 1 := GIME 36 (full red) -> 0xE0.
	esc({0x31, 1, 36});			// Palette reg1, colourValue 36
	esc({0x32, 1});				// FColor 1
	esc({0x42, 0, 210, 0, 100});		// Point (210,100)
	v.tick(1);
	CHECK(video_framebuffer[100][210] == 0xE0);

	// SetDPtr + LineM: a diagonal, and the pointer must follow.
	esc({0x32, 7});				// FColor back to white
	esc({0x40, 0, 50, 0, 200});		// SetDPtr (50,200)
	esc({0x46, 0, 60, 0, 210});		// LineM to (60,210)
	v.tick(1);
	for (int k = 0; k <= 10; k++)
		CHECK(video_framebuffer[200 + k][50 + k] == 0xFF);
	esc({0x4B, 0, 3, 0, 3});		// RBar +3,+3 from the MOVED pointer
	v.tick(1);
	CHECK(video_framebuffer[211][61] == 0xFF);	// inside (60..63,210..213)
	CHECK(video_framebuffer[213][63] == 0xFF);

	// Bar (absolute) fills the normalised rectangle.
	esc({0x40, 1, 44, 0, 150});		// SetDPtr (300,150)
	esc({0x4A, 1, 34, 0, 140});		// Bar to (290,140) — inverted corners
	v.tick(1);
	CHECK(video_framebuffer[140][290] == 0xFF);
	CHECK(video_framebuffer[150][300] == 0xFF);
	CHECK(video_framebuffer[145][295] == 0xFF);
	CHECK(video_framebuffer[139][290] == 0x00);	// outside

	// Framing: an implemented-later command (Circle, 2 params) is
	// consumed; the next byte prints as a glyph at the text cursor.
	v.write(DATA, 0x0C);
	esc({0x50, 30, 30});			// Circle — ignored, framed
	v.write(DATA, 'A');
	v.tick(1);
	CHECK(check_cell(0, 0, 'A'));

	// Framing: GPLoad's variable payload is swallowed — printable bytes
	// inside the payload must NOT render.
	esc({0x2B, 0, 1, 0, 0, 8, 0, 8, 0, 4});	// header: bufSize = 4
	v.write(DATA, 'W');			// payload...
	v.write(DATA, 'X');
	v.write(DATA, 'Y');
	v.write(DATA, 'Z');			// ...last payload byte
	v.write(DATA, 'B');			// first REAL byte again
	v.tick(1);
	CHECK(check_cell(1, 0, 'B'));
	CHECK(check_cell(2, 0, ' '));

	// Framing: a reserved sub-code is dropped without params. $27, not
	// $26 -- $26 became DWShow and now takes one, which is precisely why
	// a parameter count is not a free thing to change: this check used to
	// pass with $26 and started swallowing the byte after it.
	esc({0x27});
	v.write(DATA, 'C');
	v.tick(1);
	CHECK(check_cell(2, 0, 'C'));

	// ---- M4: guest input path (the USB keyboard's entry point) ----

	// vid_link_input feeds the RX FIFO exactly as the HID cooker does:
	// RDRF rises, reads pop in order, RDRF clears when drained.
	v.write(STAT, 0x00);			// loopback off
	v.tick(1);
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));
	vid_link_input('l');
	vid_link_input('s');
	vid_link_input(0x0D);
	v.tick(1);
	CHECK(v.read(STAT) & VID_STAT_RDRF);
	CHECK(v.read(DATA) == 'l');
	CHECK(v.read(DATA) == 's');
	CHECK(v.read(DATA) == 0x0D);
	CHECK(!(v.read(STAT) & VID_STAT_RDRF));

	// Input bytes are for the guest only — they must not render.
	v.write(DATA, 0x0C);
	vid_link_input('k');
	v.tick(1);
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
	v.tick(1);
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
	v.tick(1);
	vidterm_cursor_hide();
	CHECK(check_cell(1, 0, 'B'));   // no stale save-under painted over it

	// Turning the cursor off removes it from the framebuffer immediately.
	vidterm_cursor_service();
	v.write(DATA, 0x05);
	v.write(DATA, 0x20);
	v.tick(1);
	CHECK(!video_cursor_enabled);
	CHECK(check_cell(2, 0, ' '));   // where it had parked, now clean
	v.write(DATA, 0x05);
	v.write(DATA, 0x21);
	v.tick(1);

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
		v.tick(1);
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
		v.tick(1);
		CHECK(!(v.read(STAT) & VID_STAT_MOUSE));

		// Sticky and wheel are cleared by the latch, so a second pass
		// over an unchanged mouse reports no new press.
		vid_link_mouse_update(300, 200, VID_MOUSE_BTN_LEFT, 0);
		v.tick(1);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(s[0] == VID_MOUSE_BTN_LEFT);          // still held
		CHECK(s[1] == 0);                           // but not newly pressed

		// THE POINT OF BYTE 1: a click entirely between two polls must
		// still be seen. Press and release with no read in between.
		vid_link_mouse_update(300, 200, 0, 0);              // release
		vid_link_mouse_update(300, 200, VID_MOUSE_BTN_RIGHT, 0);
		vid_link_mouse_update(300, 200, 0, 0);              // released again
		v.tick(1);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(s[0] == 0);                           // up by the time we look
		CHECK(s[1] & VID_MOUSE_BTN_RIGHT);          // yet the press survived

		// Wheel accumulates between latches and clears after one.
		vid_link_mouse_update(300, 200, 0, 2);
		vid_link_mouse_update(300, 200, 0, -1);
		v.tick(1);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK((int8_t)s[6] == 1);
		vid_link_mouse_update(300, 200, 0, 0);
		v.tick(1);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK((int8_t)s[6] == 0);

		// Writing $FF12 must not disturb the snapshot (no scratch mirror).
		v.write(MOUSE, 0x5A);
		v.tick(1);
		CHECK(v.read(MOUSE) != 0x5A);

		// RESYNC. A guest that loses count cannot detect it -- every
		// byte is an arbitrary value -- so it must be able to regain
		// phase unconditionally and cheaply. A write to $FF12 homes the
		// index, WITHOUT the link reset's collateral damage to the
		// command and input FIFOs.
		vid_link_mouse_update(111, 222, 0, 0);
		v.tick(1);
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
		v.tick(1);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(((s[2] << 8) | s[3]) == 400);     // the sample, not the move
		CHECK(((s[4] << 8) | s[5]) == 300);

		// A soft reset also homes the index (belt and braces).
		vid_link_mouse_update(111, 222, 0, 0);
		v.tick(1);
		(void)v.read(MOUSE);
		v.write(STAT, 0x01);            // Ctl: soft reset
		v.tick(1);
		for (int i = 0; i < (int)VID_MOUSE_SNAPSHOT_LEN; i++)
			s[i] = v.read(MOUSE);
		CHECK(((s[2] << 8) | s[3]) == 111);
	}

	// ---- Pointer: drawn by the firmware, save-under restores ----
	{
		v.write(DATA, 0x0C);            // clean screen
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
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
			v.tick(1);
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

		hid_cook_set_echo(false);           // leave the suite quiet
		while (typed() != -1)               // drain what the echo typed
			;

		v.write(STAT, VID_CTL_RESET);                   // back to normal
	}

	// ---- !IRQ: RIE-gated, follows RDRF, MC6850-shaped ----
	{
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		CHECK(picovideo_irq_released());        // reset leaves it idle
		CHECK(!(v.read(STAT) & VID_STAT_IRQ));

		// A polling guest must never be interrupted by a line it did
		// not ask for: input with RIE off changes nothing.
		vid_link_input('x');
		v.tick(1);
		CHECK(v.read(STAT) & VID_STAT_RDRF);    // byte is waiting
		CHECK(picovideo_irq_released());        // but the line is idle
		CHECK(!(v.read(STAT) & VID_STAT_IRQ));

		// Enabling RIE with a byte already waiting asserts immediately.
		v.write(STAT, VID_CTL_RIE);
		v.tick(1);
		CHECK(!picovideo_irq_released());
		CHECK(v.read(STAT) & VID_STAT_IRQ);     // bit 7 mirrors the pin

		// Reading the byte drains the FIFO, so RDRF clears and the line
		// releases -- the MC6850 behaviour the guest driver expects.
		CHECK(v.read(DATA) == 'x');
		v.tick(1);
		CHECK(!(v.read(STAT) & VID_STAT_RDRF));
		CHECK(picovideo_irq_released());
		CHECK(!(v.read(STAT) & VID_STAT_IRQ));

		// More input re-asserts while RIE stays on.
		vid_link_input('y');
		v.tick(1);
		CHECK(!picovideo_irq_released());

		// Reset drops the line: it flushes the very input that
		// justified asserting, and clears RIE with it.
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		CHECK(picovideo_irq_released());
		vid_link_input('z');
		v.tick(1);
		CHECK(picovideo_irq_released());        // RIE cleared by reset
		(void)v.read(DATA);
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
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
		v.tick(1);
		CHECK(v.read(4) == 0x5A);
	}

	// M5.1: a window is a text screen that swaps into the ONE live layer.
	// The properties worth pinning are that text is CONFINED to it, that
	// switching PRESERVES what each window held, and that VID.Info reports
	// the window actually built rather than the panel.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		v.write(DATA, 0x0C);		// clear, root window
		v.tick(1);

		// A window inland: id 1, at column 4 row 2, 10 x 3.
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t dw[8] = { 1, 0, 4, 2, 10, 3, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, dw[i]);
		// Defining does not display: a guest setting up several
		// windows should not watch them flicker past. Show it.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		v.write(DATA, 'A');
		v.tick(1);

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
		v.tick(1);
		CHECK(check_cell(13, 2, 'B'));	// last column of the window
		CHECK(check_cell(4, 3, 'B'));	// wrapped to the window's row 1
		CHECK(!check_cell(14, 2, 'B'));	// never outside it

		// Select does NOT change what is displayed -- that is DWShow's
		// job. A descriptor may Select before every write burst, so a
		// visible effect here would flicker the screen once per line
		// of background output.
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		v.tick(1);
		CHECK(check_cell(4, 2, 'A'));	// still on screen

		// Showing another window is what swaps the display.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.tick(1);
		CHECK(!check_cell(4, 2, 'A'));	// window 1 is put away
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 1);
		v.tick(1);
		CHECK(check_cell(4, 2, 'A'));	// and comes back intact
		CHECK(check_cell(13, 2, 'B'));	// to the end of its first row
		CHECK(check_cell(4, 3, 'B'));	// AND on the rows below it --
						// parking only the first row
						// passes every check above

		// Selecting the root reports the panel again.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
		CHECK(!check_cell(4, 2, 'Q'));	// nothing appeared on screen
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		v.tick(1);
		CHECK(check_cell(4, 2, 'Q'));	// and it was there all along

		// Leave the console on the root window: everything after this
		// draws full-screen and would otherwise inherit a rectangle.
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		v.write(DATA, 0x0C);
		v.tick(1);
	}

	// M5.2: graphics primitives are window-RELATIVE and window-BOUNDED.
	// There is still only one graphics plane -- clipping says where a
	// primitive lands, not which surface it lands on -- so both halves
	// need asserting: that 0,0 means the window's corner, and that pixels
	// cannot escape its edges.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

		// Window at cell 4,2 = pixel 32,32; 10x3 cells = 80x48 pixels.
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t g[8] = { 1, 0, 4, 2, 10, 3, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, g[i]);
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		v.tick(1);

		// Plot at window 20,20 -- it must land 20 pixels in from the
		// WINDOW's corner (32,32), so at 52,52, not at the screen's.
		// Deliberately not 0,0: that cell holds the text cursor, whose
		// save-under and inversion would decide the pixel instead of
		// the plot, and the point here is the coordinate origin.
		v.write(DATA, ESC); v.write(DATA, 0x42);
		v.write(DATA, 0); v.write(DATA, 20);	// x = 20
		v.write(DATA, 0); v.write(DATA, 20);	// y = 20
		v.tick(1);
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
		v.tick(1);
		CHECK(video_framebuffer[40][100] != 0x00);  // inside (32..111)
		CHECK(video_framebuffer[40][112] == 0x00);  // past the right
		CHECK(video_framebuffer[80][40]  == 0x00);  // past the bottom

		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		v.write(DATA, 0x0C);
		v.tick(1);
	}

	// DWProtSw with an OUTLINE rather than a fill. Box and Bar reach the
	// framebuffer by different routes -- hline/vline against fill_rect --
	// so a clip that opens for one can stay shut for the other. These are
	// the exact bytes a guest demo sent when it reported the escape not
	// working, kept because a second path is what the Bar case misses.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t w2[8] = { 2, 0, 16, 5, 48, 16, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, w2[i]);
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 2);
		v.tick(1);
		// 1B 36 00 / 1B 32 01 / 1B 40 00 00 00 00 / 1B 48 02 58 01 F4
		v.write(DATA, ESC); v.write(DATA, 0x36); v.write(DATA, 0x00);
		v.write(DATA, ESC); v.write(DATA, 0x32); v.write(DATA, 0x01);
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0x00); v.write(DATA, 0x00);
		v.write(DATA, 0x00); v.write(DATA, 0x00);
		v.write(DATA, ESC); v.write(DATA, 0x48);
		v.write(DATA, 0x02); v.write(DATA, 0x58);
		v.write(DATA, 0x01); v.write(DATA, 0xF4);
		v.tick(1);
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
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

		// Window 1 at cell 4,2, 10x3.
		v.write(DATA, ESC); v.write(DATA, 0x20);
		const uint8_t w1[8] = { 1, 0, 4, 2, 10, 3, 0, 0 };
		for (uint8_t i = 0; i < 8; i++) v.write(DATA, w1[i]);
		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 1);
		v.tick(1);

		// Narrow the working area to 4x2 at offset 2,1 inside it, so
		// the effective rectangle is cell 6,3 and the read-back must
		// say 4x2 rather than the window's 10x3.
		v.write(DATA, ESC); v.write(DATA, 0x25);
		v.write(DATA, 2); v.write(DATA, 1);
		v.write(DATA, 4); v.write(DATA, 2);
		v.tick(1);
		v.write(3, 0);
		v.read(3); v.read(3); v.read(3); v.read(3);
		CHECK(v.read(3) == 4);		// cols of the WORKING AREA
		CHECK(v.read(3) == 2);

		// Text now starts at the working area's corner, not the
		// window's.
		v.write(DATA, 'W');
		v.tick(1);
		CHECK(check_cell(6, 3, 'W'));
		CHECK(!check_cell(4, 2, 'W'));

		// CWArea is clamped to the window just as DWSet is clamped to
		// the panel: asking for 40 columns in a 10-wide window can
		// only get 10, and the read-back says so.
		v.write(DATA, ESC); v.write(DATA, 0x25);
		v.write(DATA, 0); v.write(DATA, 0);
		v.write(DATA, 40); v.write(DATA, 40);
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
		CHECK(video_framebuffer[80][120] != 0x00);  // outside, drawn
		CHECK(video_framebuffer[20][20] == 0x00);   // still not above

		v.write(DATA, ESC); v.write(DATA, 0x26); v.write(DATA, 0);
		v.write(DATA, ESC); v.write(DATA, 0x21); v.write(DATA, 0);
		v.write(DATA, 0x0C);
		v.tick(1);
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
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

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
				v.tick(1);
		}
		for (int t = 0; t < 8; t++)
			v.tick(1);

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
		v.tick(1);

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
		v.tick(1);
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
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

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
				v.tick(1);
		}
		for (int t = 0; t < 16; t++)
			v.tick(1);

		// 1B 2D 01 01 <X16> <Y16> at 64,64
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 0x01); v.write(DATA, 0x01);
		v.write(DATA, 0x00); v.write(DATA, 0x40);
		v.write(DATA, 0x00); v.write(DATA, 0x40);
		v.tick(1);

		CHECK(video_framebuffer[64][64] == 0x20);	// first pixel
		CHECK(video_framebuffer[64][95] == 0x3F);	// end of its row
		CHECK(video_framebuffer[95][64] != 0x00);	// 32 rows deep
		CHECK(video_framebuffer[64][96] == 0x00);	// and no wider

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
		v.tick(1);
		CHECK(video_framebuffer[64][256] == 0x20);
		CHECK(video_framebuffer[64][287] == 0x3F);
	}

	// Text face, bold, and the cursor sprite. A loaded face must actually
	// replace the built-in one, and selecting buffer 0 must put it back --
	// the second half is what a one-way switch would fail.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

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
				v.tick(1);
		}
		for (int t = 0; t < 32; t++)
			v.tick(1);

		// Before selecting it, 'A' renders as 'A'.
		v.write(DATA, 0x02); v.write(DATA, 0x20); v.write(DATA, 0x20);
		v.write(DATA, 'A');
		v.tick(1);
		CHECK(check_cell(0, 0, 'A'));

		// Select the face: now every glyph is solid, so the cell is
		// full where the real 'A' has gaps.
		v.write(DATA, ESC); v.write(DATA, 0x3A);
		v.write(DATA, 2); v.write(DATA, 1);
		v.write(DATA, 0x02); v.write(DATA, 0x21); v.write(DATA, 0x20);
		v.write(DATA, 'A');
		v.tick(1);
		vidterm_cursor_hide();
		for (int gy = 0; gy < CHAR_H; gy++)
			for (int gx = 0; gx < CHAR_W; gx++)
				CHECK(video_framebuffer[gy][8 + gx] == 0xFF);

		// Buffer 0 puts the built-in face back.
		v.write(DATA, ESC); v.write(DATA, 0x3A);
		v.write(DATA, 2); v.write(DATA, 0);
		v.write(DATA, 0x02); v.write(DATA, 0x22); v.write(DATA, 0x20);
		v.write(DATA, 'A');
		v.tick(1);
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
		v.tick(1);
		CHECK(check_cell(3, 0, 'A'));	// built-in face, not garbage

		// Bold thickens a stroke: the cell gains lit pixels and keeps
		// every one it had.
		v.write(DATA, 0x0C);
		v.write(DATA, 0x02); v.write(DATA, 0x20); v.write(DATA, 0x20);
		v.write(DATA, 'H');
		v.tick(1);
		vidterm_cursor_hide();
		int plain_lit = 0;
		for (int gy = 0; gy < CHAR_H; gy++)
			for (int gx = 0; gx < CHAR_W; gx++)
				if (video_framebuffer[gy][gx] == 0xFF)
					plain_lit++;

		v.write(DATA, ESC); v.write(DATA, 0x3D); v.write(DATA, 1);
		v.write(DATA, 0x02); v.write(DATA, 0x21); v.write(DATA, 0x20);
		v.write(DATA, 'H');
		v.tick(1);
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
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

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
		v.tick(1);
		CHECK(check_cell(0, 0, 'Z'));

		// PutBlk it at 500,300 and the pixels must be what was loaded.
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 1); v.write(DATA, 0xF4);	// x = 500
		v.write(DATA, 1); v.write(DATA, 0x2C);	// y = 300
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
		CHECK(check_cell(0, 0, 'Y'));		// stream stayed in step

		const uint8_t untouched = video_framebuffer[350][800];
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 1); v.write(DATA, 3);
		v.write(DATA, 3); v.write(DATA, 0x20);	// x 800
		v.write(DATA, 1); v.write(DATA, 0x5E);	// y 350
		v.tick(1);
		CHECK(video_framebuffer[350][800] == untouched);  // nothing loaded

		// KillBuf then use it: accepted, does nothing, draws nothing.
		v.write(DATA, ESC); v.write(DATA, 0x2A);
		v.write(DATA, 1); v.write(DATA, 1);
		const uint8_t before = video_framebuffer[300][700];
		v.write(DATA, ESC); v.write(DATA, 0x2D);
		v.write(DATA, 1); v.write(DATA, 1);
		v.write(DATA, 2); v.write(DATA, 0xBC);	// x 700
		v.write(DATA, 1); v.write(DATA, 0x2C);
		v.tick(1);
		CHECK(video_framebuffer[300][700] == before);
	}

	// LSet: the raster mix mode. XOR is the one that proves it, because
	// drawing the same thing twice must return the screen to where it
	// started -- which no other mode does and no accident reproduces.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

		// A bar to combine against, then note what is under it.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		v.tick(1);
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
		v.tick(1);
		const uint8_t xored = video_framebuffer[150][150];
		CHECK(xored != under);		// something happened

		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, 0); v.write(DATA, 120);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 0); v.write(DATA, 180);
		v.write(DATA, 0); v.write(DATA, 180);
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
		const uint8_t plain = video_framebuffer[176][152];

		v.write(DATA, 0x02);			// cursor to cell 19,11
		v.write(DATA, (uint8_t)(19 + 0x20));
		v.write(DATA, (uint8_t)(11 + 0x20));
		v.write(DATA, 'M');			// a dense glyph
		v.tick(1);

		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 3);
		for (int pass = 0; pass < 2; pass++) {
			v.write(DATA, ESC); v.write(DATA, 0x40);
			v.write(DATA, 0); v.write(DATA, 150);
			v.write(DATA, 0); v.write(DATA, 170);
			v.write(DATA, ESC); v.write(DATA, 0x4A);
			v.write(DATA, 0); v.write(DATA, 170);
			v.write(DATA, 0); v.write(DATA, 190);
			v.tick(1);
		}
		v.write(DATA, ESC); v.write(DATA, 0x2F); v.write(DATA, 0);

		v.write(DATA, 0x02);			// erase the character
		v.write(DATA, (uint8_t)(19 + 0x20));
		v.write(DATA, (uint8_t)(11 + 0x20));
		v.write(DATA, ' ');
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

		// A hollow box from 100,100 to 300,250, drawn in the
		// foreground colour.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, 0); v.write(DATA, 100);
		v.write(DATA, ESC); v.write(DATA, 0x48);	// Box
		v.write(DATA, 1); v.write(DATA, 0x2C);		// 300,250
		v.write(DATA, 0); v.write(DATA, 250);
		v.tick(1);
		const uint8_t wall = video_framebuffer[100][200];
		CHECK(wall != 0x00);
		CHECK(video_framebuffer[175][200] == 0x00);	// hollow inside

		// Seed the fill in the middle of it.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 175);
		v.write(DATA, ESC); v.write(DATA, 0x4F);	// FFill
		v.tick(1);

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
		v.tick(1);
		CHECK(video_framebuffer[175][200] != 0x00);	// still here

		// And no seeds were dropped: this shape is nowhere near the
		// stack bound, so a non-zero count would mean the bound is
		// wrong rather than the shape is hard.
		uint32_t over = 1;
		vidterm_fill_stats(&over);
		CHECK(over == 0);
	}

	// Curves. The properties worth asserting are the ones a wrong
	// algorithm gets wrong: that the outline is HOLLOW and the fill is
	// not, that both are centred on the draw pointer, and that the
	// extremes land exactly a radius away rather than one pixel out.
	{
		static const uint8_t ESC = 0x1B;
		v.write(STAT, VID_CTL_RESET);
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

		// Circle of radius 40 centred at 200,200.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, 0); v.write(DATA, 200);
		v.write(DATA, ESC); v.write(DATA, 0x50);
		v.write(DATA, 0); v.write(DATA, 40);
		v.tick(1);

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
		v.tick(1);
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
		v.tick(1);
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
		v.tick(1);
		v.write(DATA, 0x0C);		// claim and clear both layers
		v.tick(1);

		// A bar across the middle of the screen, in the foreground
		// colour -- whatever the palette makes that.
		v.write(DATA, ESC); v.write(DATA, 0x40);
		v.write(DATA, 0); v.write(DATA, 64);	// SetDPtr 64,128
		v.write(DATA, 0); v.write(DATA, 128);
		v.write(DATA, ESC); v.write(DATA, 0x4A);
		v.write(DATA, 1); v.write(DATA, 0x90);	// Bar to 400,300
		v.write(DATA, 1); v.write(DATA, 44);
		v.tick(1);

		const uint8_t gfx = video_framebuffer[168][84];
		CHECK(gfx != 0x00);		// the bar really is there

		// Put a character on top of it, at cell 10,10 = px 80,160.
		v.write(DATA, 0x02);
		v.write(DATA, (uint8_t)(10 + 0x20));
		v.write(DATA, (uint8_t)(10 + 0x20));
		v.write(DATA, 'A');
		v.tick(1);

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
		v.tick(1);
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
		v.tick(1);
		v.write(DATA, 0x0C);
		v.tick(1);

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
		v.tick(1);

		v.write(DATA, 0x1B);
		v.write(DATA, 0x20);		// DWSet
		for (uint8_t p = 'A'; p <= 'H'; p++)	// 8 params
			v.write(DATA, p);
		v.write(DATA, 'Z');
		v.tick(1);
		CHECK(check_cell(0, 0, 'Z'));	// all eight swallowed
		CHECK(!check_cell(0, 0, 'H'));	// not one left over

		v.write(DATA, 0x0C);
		v.tick(1);

		v.write(DATA, 0x1B);
		v.write(DATA, 0x21);		// Select
		v.write(DATA, 'A');		// 1 param: the window id
		v.write(DATA, 'Z');
		v.tick(1);
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
		v.tick(1);
		v.write(DATA, 0x0C);		// clear and home
		v.tick(1);

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

	// M0 loopback still works after M1 (the vlink test must keep passing).
	v.write(STAT, VID_CTL_LOOPBACK);
	v.write(DATA, 0x55);
	v.tick(1);
	CHECK(v.read(STAT) & VID_STAT_RDRF);
	CHECK(v.read(DATA) == 0x55);

	if (failures == 0) {
		printf("OK\n");
		return 0;
	}
	fprintf(stderr, "%d failure(s)\n", failures);
	return 1;
}
