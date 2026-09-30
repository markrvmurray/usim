//
//	test_picortc.cpp
//	Unit tests for PicoRTC, the DS12885 at $FF32/$FF33, driven the
//	way NitrOS-9's clock2_ds12885 and the Pico firmware drive it.
//	A fake clock stands in for the host's, so every step is exact.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "picortc.h"

static int failures = 0;

#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n",		\
				__FILE__, __LINE__, #cond);		\
			++failures;					\
		}							\
	} while (0)

#define CHECK_EQ(a, b)							\
	do {								\
		const long _a = (long)(a), _b = (long)(b);		\
		if (_a != _b) {						\
			fprintf(stderr, "FAIL %s:%d: %s == %s "		\
				"($%02lX != $%02lX)\n",			\
				__FILE__, __LINE__, #a, #b, _a, _b);	\
			++failures;					\
		}							\
	} while (0)

static const Byte R_SEC = 0, R_MIN = 2, R_HOUR = 4, R_DAY = 6;
static const Byte R_DATE = 7, R_MONTH = 8, R_YEAR = 9;
static const Byte R_A = 10, R_B = 11, R_C = 12, R_D = 13;

static const Byte UIP = 0x80, SET = 0x80, UIE = 0x10, RUN = 0x06;
static const int64_t SEC = 1000000;

static int64_t now_us;

static PicoRTC::Clock fake_clock()
{
	return [] { return now_us; };
}

static Byte rd(PicoRTC& rtc, Byte reg)
{
	rtc.write(0, reg);
	return rtc.read(1);
}

static void wr(PicoRTC& rtc, Byte reg, Byte val)
{
	rtc.write(0, reg);
	rtc.write(1, val);
}

static void check_time(PicoRTC& rtc, int y, int mo, int d, int h, int mi, int s, int line)
{
	const int got[6] = { rd(rtc, R_YEAR), rd(rtc, R_MONTH), rd(rtc, R_DATE),
			     rd(rtc, R_HOUR), rd(rtc, R_MIN), rd(rtc, R_SEC) };
	const int want[6] = { y, mo, d, h, mi, s };
	if (memcmp(got, want, sizeof(got)) != 0) {
		fprintf(stderr, "FAIL %s:%d: time %02d/%02d/%02d %02d:%02d:%02d, "
			"expected %02d/%02d/%02d %02d:%02d:%02d\n", __FILE__, line,
			got[0], got[1], got[2], got[3], got[4], got[5],
			want[0], want[1], want[2], want[3], want[4], want[5]);
		++failures;
	}
}
#define CHECK_TIME(rtc, y, mo, d, h, mi, s) check_time(rtc, y, mo, d, h, mi, s, __LINE__)

// clock2_ds12885's WriteClk: freeze, write year..seconds, release
static void set_time(PicoRTC& rtc, int y, int mo, int d, int h, int mi, int s)
{
	wr(rtc, R_B, RUN | SET);
	wr(rtc, R_YEAR, y);
	wr(rtc, R_MONTH, mo);
	wr(rtc, R_DATE, d);
	wr(rtc, R_HOUR, h);
	wr(rtc, R_MIN, mi);
	wr(rtc, R_SEC, s);
	wr(rtc, R_B, RUN);
}

// A fresh chip in the emulator is a commissioned one: running, binary,
// 24-hour, valid, and on the clock's own time.
static void test_power_on()
{
	now_us = PicoRTC::civil_us(2026, 9, 30, 14, 5, 9) + SEC / 4;
	PicoRTC rtc(fake_clock());

	CHECK_TIME(rtc, 26, 9, 30, 14, 5, 9);
	CHECK_EQ(rd(rtc, R_DAY), 4);		// Wednesday, 1 = Sunday
	CHECK_EQ(rd(rtc, R_A), 0x26);		// DV = 010, not updating
	CHECK_EQ(rd(rtc, R_B), RUN);
	CHECK_EQ(rd(rtc, R_C), 0x00);
	CHECK_EQ(rd(rtc, R_D), 0x80);		// VRT: time and NVRAM valid
	CHECK_EQ(rtc.read(0), 0xFF);		// the index port is write-only
}

// The count carries through every field, including leap day and the
// year rolling over from 99 to 00.
static void test_counting()
{
	now_us = PicoRTC::civil_us(2028, 2, 28, 23, 59, 58) + SEC / 2;
	PicoRTC rtc(fake_clock());

	CHECK_TIME(rtc, 28, 2, 28, 23, 59, 58);
	CHECK_EQ(rd(rtc, R_DAY), 2);		// Monday
	now_us += 2 * SEC;
	CHECK_TIME(rtc, 28, 2, 29, 0, 0, 0);
	CHECK_EQ(rd(rtc, R_DAY), 3);
	now_us += 86400 * SEC;
	CHECK_TIME(rtc, 28, 3, 1, 0, 0, 0);

	set_time(rtc, 99, 12, 31, 23, 59, 59);
	now_us += SEC;
	CHECK_TIME(rtc, 0, 1, 1, 0, 0, 0);
}

// SetTime and GetTime round trip, as the driver does them, and SET
// holding the time still for as long as it is up.
static void test_set_freezes()
{
	now_us = PicoRTC::civil_us(2026, 1, 1, 0, 0, 0);
	PicoRTC rtc(fake_clock());

	wr(rtc, R_B, RUN | SET);
	CHECK_EQ(rd(rtc, R_A) & UIP, 0);	// no update while SET is up
	wr(rtc, R_YEAR, 27);
	wr(rtc, R_MONTH, 6);
	wr(rtc, R_DATE, 15);
	wr(rtc, R_HOUR, 12);
	wr(rtc, R_MIN, 30);
	wr(rtc, R_SEC, 45);
	now_us += 10 * SEC;
	CHECK_TIME(rtc, 27, 6, 15, 12, 30, 45);
	wr(rtc, R_B, RUN);
	CHECK_TIME(rtc, 27, 6, 15, 12, 30, 45);
	now_us += 75 * SEC;
	CHECK_TIME(rtc, 27, 6, 15, 12, 32, 0);
}

// Writing a time register with the clock running changes that field
// and the clock carries on counting from it.
static void test_write_while_running()
{
	now_us = PicoRTC::civil_us(2026, 5, 5, 10, 20, 30);
	PicoRTC rtc(fake_clock());

	wr(rtc, R_HOUR, 22);
	CHECK_TIME(rtc, 26, 5, 5, 22, 20, 30);
	now_us += 3 * SEC;
	CHECK_TIME(rtc, 26, 5, 5, 22, 20, 33);
}

// Stopping and starting keeps the place within the second, so the
// next update still falls where it would have.
static void test_phase_kept()
{
	now_us = PicoRTC::civil_us(2026, 3, 3, 3, 3, 3) + SEC * 9 / 10;
	PicoRTC rtc(fake_clock());

	wr(rtc, R_B, RUN | SET);
	now_us += 5 * SEC;
	wr(rtc, R_B, RUN);
	CHECK_EQ(rd(rtc, R_SEC), 3);
	now_us += SEC / 20;			// 0.95 into the second
	CHECK_EQ(rd(rtc, R_SEC), 3);
	now_us += SEC / 10;			// past it
	CHECK_EQ(rd(rtc, R_SEC), 4);
}

// UIP covers the 244us before each update and the update itself.
static void test_uip()
{
	const int64_t base = PicoRTC::civil_us(2026, 7, 7, 7, 7, 7);
	now_us = base;
	PicoRTC rtc(fake_clock());

	now_us = base + SEC / 2;
	CHECK_EQ(rd(rtc, R_A) & UIP, 0);
	now_us = base + SEC - 245;
	CHECK_EQ(rd(rtc, R_A) & UIP, 0);
	now_us = base + SEC - 244;
	CHECK_EQ(rd(rtc, R_A) & UIP, UIP);
	now_us = base + SEC + 1983;
	CHECK_EQ(rd(rtc, R_A) & UIP, UIP);
	now_us = base + SEC + 1984;
	CHECK_EQ(rd(rtc, R_A) & UIP, 0);

	wr(rtc, R_A, 0xA6);			// UIP is read-only
	CHECK_EQ(rd(rtc, R_A), 0x26);
}

// Only DV = 010 keeps time; 11x holds the divider in reset and the
// first update comes 500ms after release.
static void test_divider()
{
	now_us = PicoRTC::civil_us(2026, 8, 8, 8, 8, 8) + SEC / 10;
	PicoRTC rtc(fake_clock());

	wr(rtc, R_A, 0x06);			// oscillator off
	now_us += 60 * SEC;
	CHECK_TIME(rtc, 26, 8, 8, 8, 8, 8);
	CHECK_EQ(rd(rtc, R_A) & UIP, 0);

	wr(rtc, R_A, 0x76);			// divider in reset
	now_us += 60 * SEC;
	wr(rtc, R_A, 0x26);
	now_us += SEC * 49 / 100;
	CHECK_EQ(rd(rtc, R_SEC), 8);
	now_us += SEC * 2 / 100;
	CHECK_EQ(rd(rtc, R_SEC), 9);
}

// BCD and 12-hour modes, including the noon and midnight edges.
// Changing the mode reinterprets the registers, as on the chip.
static void test_bcd_12_hour()
{
	now_us = PicoRTC::civil_us(2026, 1, 1, 0, 0, 0);
	PicoRTC rtc(fake_clock());

	wr(rtc, R_B, SET);			// BCD, 12-hour
	wr(rtc, R_YEAR, 0x26);
	wr(rtc, R_MONTH, 0x12);
	wr(rtc, R_DATE, 0x31);
	wr(rtc, R_HOUR, 0x11);			// 11 AM
	wr(rtc, R_MIN, 0x59);
	wr(rtc, R_SEC, 0x58);
	wr(rtc, R_B, 0x00);
	now_us += 3 * SEC;
	CHECK_TIME(rtc, 0x26, 0x12, 0x31, 0x92, 0x00, 0x01);	// 12:00:01 PM
	now_us += 12 * 3600 * SEC;
	CHECK_TIME(rtc, 0x27, 0x01, 0x01, 0x12, 0x00, 0x01);	// 12:00:01 AM
	now_us += 3600 * SEC;
	CHECK_EQ(rd(rtc, R_HOUR), 0x01);

	wr(rtc, R_B, RUN);			// 0x27 is now read as 39
	CHECK_EQ(rd(rtc, R_YEAR), 39);
}

// The day of the week is its own counter: writing the date alone
// leaves it counting from where it was.
static void test_day_of_week()
{
	now_us = PicoRTC::civil_us(2026, 9, 30, 23, 59, 59);	// Wednesday
	PicoRTC rtc(fake_clock());

	wr(rtc, R_DAY, 1);
	CHECK_EQ(rd(rtc, R_DAY), 1);
	now_us += SEC;
	CHECK_EQ(rd(rtc, R_DAY), 2);
	wr(rtc, R_DATE, 5);
	CHECK_EQ(rd(rtc, R_DAY), 2);
	now_us += 5 * 86400 * SEC;
	CHECK_EQ(rd(rtc, R_DAY), 7);
	now_us += 86400 * SEC;
	CHECK_EQ(rd(rtc, R_DAY), 1);
}

// C and D are read-only; SET clears UIE; the index is seven bits.
static void test_registers()
{
	now_us = PicoRTC::civil_us(2026, 1, 1, 0, 0, 0);
	PicoRTC rtc(fake_clock());

	wr(rtc, R_C, 0xFF);
	wr(rtc, R_D, 0x00);
	CHECK_EQ(rd(rtc, R_C), 0x00);
	CHECK_EQ(rd(rtc, R_D), 0x80);

	wr(rtc, R_B, UIE | RUN);
	CHECK_EQ(rd(rtc, R_B), UIE | RUN);
	wr(rtc, R_B, SET | UIE | RUN);
	CHECK_EQ(rd(rtc, R_B), SET | RUN);

	wr(rtc, 0x80 | 0x0E, 0xA5);
	CHECK_EQ(rd(rtc, 0x0E), 0xA5);
	wr(rtc, 0x7F, 0x5A);
	CHECK_EQ(rd(rtc, 0x7F), 0x5A);
	wr(rtc, 0x01, 0x33);			// alarm registers are plain storage
	CHECK_EQ(rd(rtc, 0x01), 0x33);
}

// The battery: the NVRAM and a set time survive the emulator stopping,
// and the clock kept counting while it was off. ~RESET at power-on
// clears the interrupt enables but nothing else.
static void test_persistence()
{
	char path[] = "/tmp/picortc_test_XXXXXX";
	const int fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); exit(EXIT_FAILURE); }
	close(fd);
	unlink(path);			// start from no file at all

	now_us = PicoRTC::civil_us(2026, 4, 1, 9, 0, 0);
	{
		PicoRTC rtc(fake_clock(), path);
		set_time(rtc, 30, 12, 25, 18, 0, 0);
		wr(rtc, 0x40, 0xC3);
		wr(rtc, R_B, UIE | RUN);
	}
	now_us += 3600 * SEC;
	{
		PicoRTC rtc(fake_clock(), path);
		CHECK_TIME(rtc, 30, 12, 25, 19, 0, 0);
		CHECK_EQ(rd(rtc, 0x40), 0xC3);
		CHECK_EQ(rd(rtc, R_B), RUN);
		wr(rtc, R_B, RUN | SET);	// stopped, and stays stopped
	}
	now_us += 3600 * SEC;
	{
		PicoRTC rtc(fake_clock(), path);
		CHECK_TIME(rtc, 30, 12, 25, 19, 0, 0);
	}
	unlink(path);
}

int main()
{
	test_power_on();
	test_counting();
	test_set_freezes();
	test_write_while_running();
	test_phase_kept();
	test_uip();
	test_divider();
	test_bcd_12_hour();
	test_day_of_week();
	test_registers();
	test_persistence();

	if (failures) {
		printf("test_picortc: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_picortc: PASS\n");
	return EXIT_SUCCESS;
}
