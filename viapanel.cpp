//
//	viapanel.cpp
//	Front panel for the VIA at $FF20 (see viapanel.h)
//
//	vim: ts=8 sw=8 noet:
//

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <utility>

#include "viapanel.h"
#include "sdlhost.h"

static const uint32_t	PRESENT_FLOOR_MS = 30;	// ~33 fps cap

// Layout, in unscaled pixels. Each port is a row of ten columns, PA7
// down to PA0 then CA1 and CA2, each with its name, an LED and a switch.
static const int	MARGIN = 8;
static const int	COL_W = 22;
static const int	GAP = 10;	// between the port pins and the control lines
static const int	TITLE_H = 22;
static const int	ROW_H = 64;
static const int	LEGEND_H = 18;
static const int	WIDTH = 2 * MARGIN + 10 * COL_W + GAP;
static const int	HEIGHT = TITLE_H + 2 * ROW_H + LEGEND_H;

static const int	LED_Y = 18;	// offsets within a port's row
static const int	LED_R = 5;
static const int	SW_Y = 28;
static const int	SW_H = 24;
static const int	PUSH_COL = 8;	// C?1: a push button

static int col_x(int col)
{
	return MARGIN + col * COL_W + (col >= 8 ? GAP : 0) + COL_W / 2;
}

static int row_y(int port)
{
	return TITLE_H + port * ROW_H;
}

//----------------------------------------------------------------------------
// Drawing
//----------------------------------------------------------------------------

struct rgb { Uint8 r, g, b; };

static const rgb	BACKGROUND	= {  24,  24,  28 };
static const rgb	TEXT		= { 200, 200, 200 };
static const rgb	OUT_ON		= { 255,  40,  40 };
static const rgb	OUT_OFF		= {  70,  12,  12 };
static const rgb	IN_ON		= {  40, 220,  60 };
static const rgb	IN_OFF		= {  12,  60,  18 };
static const rgb	IRQ_ON		= { 255, 170,   0 };
static const rgb	IRQ_OFF		= {  70,  45,   0 };
static const rgb	SW_BODY		= {  90,  90,  96 };
static const rgb	SW_KNOB		= { 230, 230, 230 };
static const rgb	SW_BODY_DIM	= {  45,  45,  50 };
static const rgb	SW_KNOB_DIM	= { 100, 100, 100 };

// 5x7 glyphs for the labels, a row to a byte, bit 4 leftmost
struct glyph { char c; Uint8 rows[7]; };

static const glyph	font[] = {
	{ '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
	{ '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
	{ '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
	{ '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
	{ '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
	{ '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
	{ '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
	{ 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
	{ 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
	{ 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
	{ 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
	{ 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
	{ '$', { 0x04, 0x0f, 0x14, 0x0e, 0x05, 0x1e, 0x04 } },
};

static void colour(SDL_Renderer* ren, rgb c)
{
	SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 0xff);
}

static void fill(SDL_Renderer* ren, int x, int y, int w, int h)
{
	SDL_Rect r = { x, y, w, h };
	SDL_RenderFillRect(ren, &r);
}

// characters without a glyph are left blank
static void text(SDL_Renderer* ren, int x, int y, const char* s, rgb c)
{
	colour(ren, c);
	for (; *s != '\0'; s++, x += 6)
		for (const glyph& g : font) {
			if (g.c != *s)
				continue;
			for (int row = 0; row < 7; row++)
				for (int bit = 0; bit < 5; bit++)
					if (g.rows[row] & (0x10 >> bit))
						fill(ren, x + bit, y + row, 1, 1);
			break;
		}
}

static void disc(SDL_Renderer* ren, int cx, int cy, int r, rgb c)
{
	colour(ren, c);
	for (int dy = -r; dy <= r; dy++) {
		const int dx = (int)std::lround(std::sqrt((double)(r * r - dy * dy)));
		fill(ren, cx - dx, cy + dy, 2 * dx + 1, 1);
	}
}

static void led(SDL_Renderer* ren, int cx, int cy, bool level, bool driven)
{
	disc(ren, cx, cy, LED_R, driven ? (level ? OUT_ON : OUT_OFF)
					: (level ? IN_ON : IN_OFF));
}

// a toggle: the knob up for high
static void toggle(SDL_Renderer* ren, int cx, int top, bool up, bool dim)
{
	colour(ren, dim ? SW_BODY_DIM : SW_BODY);
	fill(ren, cx - 5, top, 10, SW_H);
	colour(ren, dim ? SW_KNOB_DIM : SW_KNOB);
	fill(ren, cx - 4, up ? top + 1 : top + SW_H / 2, 8, SW_H / 2 - 1);
}

// a push button, sunk while held
static void button(SDL_Renderer* ren, int cx, int top, bool held, bool dim)
{
	colour(ren, dim ? SW_BODY_DIM : SW_BODY);
	fill(ren, cx - 7, top + 5, 14, 14);
	colour(ren, dim ? SW_KNOB_DIM : (held ? SW_BODY_DIM : SW_KNOB));
	fill(ren, cx - 5, top + 7, 10, 10);
}

void ViaPanel::draw()
{
	const mos6522& v = *via;

	colour(ren, BACKGROUND);
	SDL_RenderClear(ren);

	text(ren, MARGIN, 7, "VIA $FF20", TEXT);
	text(ren, WIDTH - MARGIN - 2 * LED_R - 22, 7, "IRQ", TEXT);
	disc(ren, WIDTH - MARGIN - LED_R, 10, LED_R, v.irq() ? IRQ_ON : IRQ_OFF);

	for (int port = 0; port < 2; port++) {
		const int top = row_y(port);
		const char p = port == 0 ? 'A' : 'B';
		const Byte pins = port == 0 ? v.pa() : v.pb();
		const Byte driven = port == 0 ? v.pa_driven() : v.pb_driven();
		const Byte sw = port == 0 ? sw_a : sw_b;

		for (int col = 0; col < 8; col++) {
			const int bit = 7 - col;
			const int cx = col_x(col);
			const char name[] = { 'P', p, (char)('0' + bit), '\0' };
			const bool out = (driven >> bit) & 1;

			text(ren, cx - 8, top + 2, name, TEXT);
			led(ren, cx, top + LED_Y, (pins >> bit) & 1, out);
			toggle(ren, cx, top + SW_Y, (sw >> bit) & 1, out);
		}

		const bool c1 = port == 0 ? v.ca1() : v.cb1();
		const bool c1_out = port == 0 ? false : v.cb1_driven();
		const bool c2 = port == 0 ? v.ca2() : v.cb2();
		const bool c2_out = port == 0 ? v.ca2_driven() : v.cb2_driven();
		const bool sw2 = port == 0 ? sw_ca2 : sw_cb2;
		const char name1[] = { 'C', p, '1', '\0' };
		const char name2[] = { 'C', p, '2', '\0' };
		int cx = col_x(PUSH_COL);

		text(ren, cx - 8, top + 2, name1, TEXT);
		led(ren, cx, top + LED_Y, c1, c1_out);
		button(ren, cx, top + SW_Y, held == port, c1_out);

		cx = col_x(PUSH_COL + 1);
		text(ren, cx - 8, top + 2, name2, TEXT);
		led(ren, cx, top + LED_Y, c2, c2_out);
		toggle(ren, cx, top + SW_Y, sw2, c2_out);
	}

	const int y = row_y(2) + 6;
	disc(ren, MARGIN + 3, y + 3, 3, OUT_ON);
	text(ren, MARGIN + 10, y, "OUT", TEXT);
	disc(ren, MARGIN + 37, y + 3, 3, IN_ON);
	text(ren, MARGIN + 44, y, "IN", TEXT);

	SDL_RenderPresent(ren);
}

//----------------------------------------------------------------------------
// The switches
//----------------------------------------------------------------------------

// The setters see only changes, so pushing every switch at once is
// harmless, and only the ones that moved make edges.
void ViaPanel::apply()
{
	via->set_pa(sw_a);
	via->set_pb(sw_b);
	via->set_ca1(sw_ca1);
	via->set_ca2(sw_ca2);
	via->set_cb1(sw_cb1);
	via->set_cb2(sw_cb2);
}

void ViaPanel::click(int x, int y)
{
	for (int port = 0; port < 2; port++) {
		const int top = row_y(port) + SW_Y;
		if (y < top || y >= top + SW_H)
			continue;
		for (int col = 0; col < 10; col++) {
			const int cx = col_x(col);
			if (x < cx - COL_W / 2 || x >= cx + COL_W / 2)
				continue;
			Byte& sw = port == 0 ? sw_a : sw_b;
			if (col < 8)
				sw ^= 0x80 >> col;
			else if (col == PUSH_COL) {
				(port == 0 ? sw_ca1 : sw_cb1) = false;
				held = port;
			} else {
				bool& c2 = port == 0 ? sw_ca2 : sw_cb2;
				c2 = !c2;
			}
			apply();
			return;
		}
	}
}

void ViaPanel::release()
{
	if (held < 0)
		return;
	(held == 0 ? sw_ca1 : sw_cb1) = true;
	held = -1;
	apply();
}

//----------------------------------------------------------------------------
// The window
//----------------------------------------------------------------------------

ViaPanel::ViaPanel(USim& sys, std::shared_ptr<mos6522> via, int scale)
	: sys(sys), via(std::move(via)), scale(scale < 1 ? 1 : scale)
{
}

ViaPanel::~ViaPanel()
{
	close_window();
}

// the switches are the outside world: a reset leaves them alone
void ViaPanel::reset()
{
}

bool ViaPanel::open_window()
{
	if (!sdlhost_acquire())
		return false;
	inited = true;

	win = SDL_CreateWindow("VIA $FF20",
	    SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	    WIDTH * scale, HEIGHT * scale, 0);
	if (win == nullptr) {
		fprintf(stderr, "viapanel: SDL_CreateWindow failed: %s\n", SDL_GetError());
		return false;
	}
	ren = SDL_CreateRenderer(win, -1, 0);
	if (ren == nullptr) {
		fprintf(stderr, "viapanel: SDL_CreateRenderer failed: %s\n", SDL_GetError());
		return false;
	}
	// the logical size scales the mouse back down for click() too
	SDL_RenderSetLogicalSize(ren, WIDTH, HEIGHT);
	sdlhost_listen(SDL_GetWindowID(win), [this](const SDL_Event& ev) {
		on_event(ev);
	});
	apply();
	return true;
}

void ViaPanel::close_window()
{
	if (ren != nullptr) { SDL_DestroyRenderer(ren); ren = nullptr; }
	if (win != nullptr) {
		sdlhost_ignore(SDL_GetWindowID(win));
		SDL_DestroyWindow(win);
		win = nullptr;
	}
	if (inited) { sdlhost_release(); inited = false; }
}

void ViaPanel::on_event(const SDL_Event& ev)
{
	switch (ev.type) {
	case SDL_QUIT:
		quit = true;
		break;
	case SDL_WINDOWEVENT:
		if (ev.window.event == SDL_WINDOWEVENT_CLOSE)
			closed = true;
		break;
	case SDL_MOUSEBUTTONDOWN:
		if (ev.button.button == SDL_BUTTON_LEFT)
			click(ev.button.x, ev.button.y);
		break;
	case SDL_MOUSEBUTTONUP:
		if (ev.button.button == SDL_BUTTON_LEFT)
			release();
		break;
	default:
		break;
	}
}

void ViaPanel::tick(uint32_t ticks)
{
	cycles += ticks;
	if (cycles < SERVICE_INTERVAL)
		return;
	cycles = 0;

	if (closed)
		return;
	if (!opened) {
		opened = true;	// one attempt; failure leaves the VIA unattended
		if (!open_window()) {
			close_window();
			closed = true;
			return;
		}
	}

	sdlhost_pump();
	if (quit) {
		sys.halt();
		return;
	}
	if (closed) {
		release();	// a button held as the window went
		close_window();
		return;
	}

	// Wall-clock floor: the guest-cycle interval fires far faster than
	// 33 fps when the emulator free-runs.
	const uint32_t now = SDL_GetTicks();
	if (now - last_present_ms < PRESENT_FLOOR_MS)
		return;
	last_present_ms = now;
	draw();
}
