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

	// Framing: a reserved sub-code is dropped without params.
	esc({0x26});
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

		// Unmapped usages produce nothing at all (F1 here).
		kbd(0x00, 0x3A); released(); CHECK(typed() == -1);

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
