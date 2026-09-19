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

#include "mc6809.h"
#include "picovideo.h"
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
