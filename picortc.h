//
//	picortc.h
//	Pico-Thing DS12885 Real-Time Clock
//
//	Two addresses at $FF32-$FF33, Motorola-mode two-step access:
//	  $FF32 (index): write the register number, $00-$7F
//	  $FF33 (data):  read or write the selected register
//
//	128 registers, MC146818 compatible:
//	  $00-$09  seconds, alarm, minutes, alarm, hours, alarm,
//	           day of week (1 = Sunday), date, month, year (00-99)
//	  $0A      A: UIP (bit 7, read-only), DV2-0, RS3-0
//	  $0B      B: SET, PIE, AIE, UIE, SQWE, DM, 24/12, DSE
//	  $0C      C: interrupt flags (read-only)
//	  $0D      D: VRT (bit 7, read-only)
//	  $0E-$7F  general-purpose NVRAM
//
//	The clock counts from a caller-supplied source of wall-clock
//	microseconds, so it can follow the host's clock or the guest's
//	cycle count. Time is held as an offset from that source, which is
//	also what makes a persisted clock keep counting while the
//	emulator is not running, as the battery-backed chip does.
//
//	The clock runs when DV2-0 = 010 and SET = 0. Stopping it freezes
//	the time registers and the position within the second; starting
//	it again carries on from both. Holding the divider in reset
//	(DV2-0 = 11x) leaves the first update 500ms after release.
//
//	Binary/BCD and 12/24-hour modes are honoured. As on the chip,
//	changing either mode reinterprets the time registers rather than
//	converting them, and the day of the week counts on its own rather
//	than being derived from the date.
//
//	Not modelled: alarms, periodic and update-ended interrupts (the
//	flags in C always read zero, and no IRQ is driven: the board does
//	not route one), the square-wave output, and daylight saving.
//	VRT always reads valid.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <cstdint>
#include <functional>
#include "device.h"

class PicoRTC : public MappedDevice {

public:
	// wall-clock microseconds, in local time
	using Clock = std::function<int64_t()>;

	static const size_t	RTC_SIZE = 128;

protected:
	Clock			clock;
	const char*		filepath;
	Byte			index;
	Byte			ram[RTC_SIZE];
	int64_t			offset_us;	// RTC time minus clock(), while running
	uint32_t		frac_us;	// position within the second, while stopped
	int			dow_delta;	// day-of-week register minus the date's

	bool			running() const;
	void			latch(int64_t now);
	void			resync(int64_t now);
	void			encode(int64_t us);
	int64_t			decode() const;

public:
			PicoRTC(Clock clock, const char* filepath = nullptr);
	virtual		~PicoRTC() = default;

	Byte		read(Word offset) override;
	void		write(Word offset, Byte val) override;

	void		load();
	void		save();

	// local time from a civil date, and the host's clock, in microseconds
	static int64_t	civil_us(int year, int month, int day, int hour, int min, int sec);
	static int64_t	host_local_us();
};
