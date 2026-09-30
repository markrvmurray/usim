//
//	picortc.cpp
//	Pico-Thing DS12885 Real-Time Clock
//
//	vim: ts=8 sw=8 noet:
//

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "picortc.h"

static const Byte	REG_SEC   = 0x00;
static const Byte	REG_MIN   = 0x02;
static const Byte	REG_HOUR  = 0x04;
static const Byte	REG_DAY   = 0x06;
static const Byte	REG_DATE  = 0x07;
static const Byte	REG_MONTH = 0x08;
static const Byte	REG_YEAR  = 0x09;
static const Byte	REG_A     = 0x0A;
static const Byte	REG_B     = 0x0B;
static const Byte	REG_C     = 0x0C;
static const Byte	REG_D     = 0x0D;

static const Byte	A_UIP     = 0x80;
static const Byte	A_DV      = 0x70;
static const Byte	A_DV_RUN  = 0x20;	// 010, the only pattern that keeps time
static const Byte	A_DV_RST  = 0x60;	// 11x holds the divider in reset
static const Byte	B_SET     = 0x80;
static const Byte	B_UIE     = 0x10;
static const Byte	B_RESET   = 0x78;	// PIE, AIE, UIE, SQWE: cleared by ~RESET
static const Byte	B_DM      = 0x04;	// 1 = binary
static const Byte	B_24      = 0x02;	// 1 = 24-hour
static const Byte	D_VRT     = 0x80;

static const int64_t	US        = 1000000;
static const int64_t	UIP_LEAD  = 244;	// UIP rises this long before an update
static const int64_t	UIP_LEN   = 1984;	// and stays up while the update runs

static const char	MAGIC[4]  = { 'R', 'T', 'C', '1' };

static int64_t floor_div(int64_t a, int64_t b)
{
	int64_t q = a / b;
	return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static int64_t floor_mod(int64_t a, int64_t b)
{
	return a - floor_div(a, b) * b;
}

// days since 1970-01-01 (H. Hinnant's civil calendar algorithms)
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
	y -= m <= 2;
	const int64_t era = floor_div(y, 400);
	const int64_t yoe = y - era * 400;
	const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int& y, int& m, int& d)
{
	z += 719468;
	const int64_t era = floor_div(z, 146097);
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	d = (int)(doy - (153 * mp + 2) / 5 + 1);
	m = (int)(mp < 10 ? mp + 3 : mp - 9);
	y = (int)(yoe + era * 400 + (m <= 2));
}

// 0 = Sunday; 1970-01-01 was a Thursday
static int weekday(int64_t days)
{
	return (int)floor_mod(days + 4, 7);
}

int64_t PicoRTC::civil_us(int year, int month, int day, int hour, int min, int sec)
{
	return (days_from_civil(year, month, day) * 86400
		+ hour * 3600 + min * 60 + sec) * US;
}

int64_t PicoRTC::host_local_us()
{
	using namespace std::chrono;
	const int64_t now = duration_cast<microseconds>(
		system_clock::now().time_since_epoch()).count();
	const time_t secs = (time_t)floor_div(now, US);
	struct tm tm;
	localtime_r(&secs, &tm);
	return civil_us(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec) + floor_mod(now, US);
}

PicoRTC::PicoRTC(Clock clock, const char* filepath)
	: clock(std::move(clock)), filepath(filepath), index(0),
	  offset_us(0), frac_us(0), dow_delta(0)
{
	// a commissioned chip: running, binary, 24-hour, on the clock's time
	memset(ram, 0, RTC_SIZE);
	ram[REG_A] = A_DV_RUN | 0x06;
	ram[REG_B] = B_DM | B_24;
	ram[REG_D] = D_VRT;
	load();

	// the board's power-on ~RESET
	ram[REG_B] &= ~B_RESET;
	ram[REG_C] = 0;
}

bool PicoRTC::running() const
{
	return (ram[REG_A] & A_DV) == A_DV_RUN && !(ram[REG_B] & B_SET);
}

static Byte to_reg(int v, bool bin)
{
	return bin ? (Byte)v : (Byte)(((v / 10) << 4) | (v % 10));
}

static int from_reg(Byte b, bool bin)
{
	return bin ? b : (b >> 4) * 10 + (b & 0x0F);
}

// write the time at us into the time registers, keeping the sub-second
void PicoRTC::encode(int64_t us)
{
	const bool bin = ram[REG_B] & B_DM;
	const int64_t secs = floor_div(us, US);
	const int64_t days = floor_div(secs, 86400);
	const int sod = (int)(secs - days * 86400);
	int y, m, d;
	civil_from_days(days, y, m, d);

	int hour = sod / 3600;
	Byte pm = 0;
	if (!(ram[REG_B] & B_24)) {
		pm = hour >= 12 ? 0x80 : 0;
		hour %= 12;
		if (hour == 0)
			hour = 12;
	}

	ram[REG_SEC]   = to_reg(sod % 60, bin);
	ram[REG_MIN]   = to_reg(sod / 60 % 60, bin);
	ram[REG_HOUR]  = to_reg(hour, bin) | pm;
	ram[REG_DAY]   = to_reg(1 + (int)floor_mod(weekday(days) + dow_delta, 7), bin);
	ram[REG_DATE]  = to_reg(d, bin);
	ram[REG_MONTH] = to_reg(m, bin);
	ram[REG_YEAR]  = to_reg((int)floor_mod(y - 2000, 100), bin);
	frac_us = (uint32_t)(us - secs * US);
}

// the time in the time registers, in whole seconds
int64_t PicoRTC::decode() const
{
	const bool bin = ram[REG_B] & B_DM;
	int hour;
	if (ram[REG_B] & B_24) {
		hour = from_reg(ram[REG_HOUR], bin);
	} else {
		hour = from_reg(ram[REG_HOUR] & 0x7F, bin) % 12;
		if (ram[REG_HOUR] & 0x80)
			hour += 12;
	}
	return days_from_civil(2000 + from_reg(ram[REG_YEAR], bin),
			       from_reg(ram[REG_MONTH], bin),
			       from_reg(ram[REG_DATE], bin)) * 86400
		+ hour * 3600
		+ from_reg(ram[REG_MIN], bin) * 60
		+ from_reg(ram[REG_SEC], bin);
}

// freeze the running time into the registers
void PicoRTC::latch(int64_t now)
{
	if (running())
		encode(now + offset_us);
}

// restart the count from the registers
void PicoRTC::resync(int64_t now)
{
	if (!running())
		return;
	const int64_t secs = decode();
	const bool bin = ram[REG_B] & B_DM;
	dow_delta = (int)floor_mod(from_reg(ram[REG_DAY], bin) - 1
				   - weekday(floor_div(secs, 86400)), 7);
	offset_us = secs * US + frac_us - now;
}

Byte PicoRTC::read(Word offset)
{
	if (offset != 1)
		return 0xFF;	// the index is write-only

	switch (index) {
	case REG_SEC: case REG_MIN: case REG_HOUR: case REG_DAY:
	case REG_DATE: case REG_MONTH: case REG_YEAR:
		latch(clock());
		return ram[index];
	case REG_A: {
		Byte a = ram[REG_A] & ~A_UIP;
		if (running()) {
			const int64_t frac = floor_mod(clock() + offset_us, US);
			if (frac >= US - UIP_LEAD || frac < UIP_LEN)
				a |= A_UIP;
		}
		return a;
	}
	case REG_C:
		return 0;
	case REG_D:
		return D_VRT;
	default:
		return ram[index];
	}
}

void PicoRTC::write(Word offset, Byte val)
{
	if (offset == 0) {
		index = val & (RTC_SIZE - 1);
		return;
	}
	if (index == REG_C || index == REG_D)
		return;		// read-only

	// latch then resync is lossless, so any write can stop, restart
	// or alter the clock without disturbing its place in the second
	const int64_t now = clock();
	latch(now);
	switch (index) {
	case REG_A:
		ram[REG_A] = val & ~A_UIP;
		if ((val & A_DV_RST) == A_DV_RST)
			frac_us = US / 2;	// first update 500ms after release
		break;
	case REG_B:
		ram[REG_B] = (val & B_SET) ? (val & ~B_UIE) : val;
		break;
	default:
		ram[index] = val;
		break;
	}
	resync(now);
	save();
}

static void put64(uint8_t* p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get64(const uint8_t* p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

// file layout: magic, the 128 registers, offset_us, frac_us, dow_delta
static const size_t	FILE_SIZE = sizeof(MAGIC) + PicoRTC::RTC_SIZE + 8 + 8 + 1;

void PicoRTC::load()
{
	if (!filepath) return;
	FILE* fp = fopen(filepath, "rb");
	if (!fp) return;
	uint8_t buf[FILE_SIZE];
	const size_t n = fread(buf, 1, FILE_SIZE, fp);
	fclose(fp);
	if (n != FILE_SIZE || memcmp(buf, MAGIC, sizeof(MAGIC)) != 0)
		return;
	const uint8_t* p = buf + sizeof(MAGIC);
	memcpy(ram, p, RTC_SIZE);
	p += RTC_SIZE;
	offset_us = (int64_t)get64(p);
	frac_us = (uint32_t)get64(p + 8) % US;
	dow_delta = p[16] % 7;
}

void PicoRTC::save()
{
	if (!filepath) return;
	uint8_t buf[FILE_SIZE];
	uint8_t* p = buf;
	memcpy(p, MAGIC, sizeof(MAGIC));
	p += sizeof(MAGIC);
	memcpy(p, ram, RTC_SIZE);
	p += RTC_SIZE;
	put64(p, (uint64_t)offset_us);
	put64(p + 8, frac_us);
	p[16] = (uint8_t)dow_delta;
	FILE* fp = fopen(filepath, "wb");
	if (fp) {
		(void)fwrite(buf, 1, FILE_SIZE, fp);
		fclose(fp);
	}
}
