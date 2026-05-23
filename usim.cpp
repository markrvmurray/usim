//
//	usim.cpp
//	(C) R.P.Bellis 1994 - 2025
//
//      vim: ts=8 sw=8 noet:
//

#include <cstdlib>
#include <cstdio>
#include "usim.h"

//----------------------------------------------------------------------------
// Generic processor run state routines
//----------------------------------------------------------------------------

void USim::run()
{
	halted = false;
	while (!halted) {
		tick();
	}
}

void USim::tick()
{
	// assume one cycle happens every time
	++cycles;

	// update all active devices
	for (auto& d : dev_active) {
		d.device->tick(cycles);
	}

	// accumulate canonical cycle count then reset the per-tick counter
	total_cycles += cycles;
	cycles = 0;
}

void USim::reset()
{
	// reset all active devices
	for (auto& d : dev_active) {
		d.device->reset();
	}
}

void USim::halt()
{
	halted = true;
}

void USim::invalid(const char *msg)
{
	fprintf(stderr, "error: %s [PC:$%04X IR:$%04X]\n", msg, pc, ir);
	this->abort();
}

Byte USim::fetch()
{
	return read(pc++);
}

//----------------------------------------------------------------------------
// Device handling
//----------------------------------------------------------------------------

void USim::attach(const ActiveDevice::shared_ptr& dev)
{
	dev_active.push_back({ dev });
}

void USim::attach(const MappedDevice::shared_ptr& dev, Word base, Word mask, rank<0>)
{
	dev_mapped.push_back({ dev, base, mask, 0 });
}

void USim::attach(const ActiveMappedDevice::shared_ptr& dev, Word base, Word mask, rank<1>)
{
	dev_active.push_back({ dev });
	dev_mapped.push_back({ dev, base, mask, 0 });
}

void USim::attach_range(const MappedDevice::shared_ptr& dev, Word base, Word size, rank<0>)
{
	dev_mapped.push_back({ dev, base, 0, size });
}

void USim::attach_range(const ActiveMappedDevice::shared_ptr& dev, Word base, Word size, rank<1>)
{
	dev_active.push_back({ dev });
	dev_mapped.push_back({ dev, base, 0, size });
}

//----------------------------------------------------------------------------
// Mapped Device IO
//----------------------------------------------------------------------------

// Single byte read
Byte USim::read(Word offset)
{
	++cycles;
	for (auto& d : dev_mapped) {
		// Range-mode uses 32-bit math on the upper bound so a range
		// that reaches $FFFF (e.g. pico-thing's FRAM at $FFD0-$FFFF)
		// doesn't wrap to 0 and silently miss every address.
		bool hit = d.size
			? (offset >= d.base &&
			   (uint32_t)offset < (uint32_t)d.base + (uint32_t)d.size)
			: ((offset & d.mask) == d.base);
		if (hit) {
			return d.device->read(offset - d.base);
		}
	}
	return 0xff;
}

// Single byte write
void USim::write(Word offset, Byte val)
{
	++cycles;
	for (auto& d : dev_mapped) {
		// Range-mode uses 32-bit math on the upper bound so a range
		// that reaches $FFFF (e.g. pico-thing's FRAM at $FFD0-$FFFF)
		// doesn't wrap to 0 and silently miss every address.
		bool hit = d.size
			? (offset >= d.base &&
			   (uint32_t)offset < (uint32_t)d.base + (uint32_t)d.size)
			: ((offset & d.mask) == d.base);
		if (hit) {
			d.device->write(offset - d.base, val);
			break;
		}
	}
}

//----------------------------------------------------------------------------
// Word memory access routines for big-endian (Motorola type)
//----------------------------------------------------------------------------

Word USimBE::fetch_word()
{
	Word		tmp;

	tmp  = fetch() << 8;
	tmp |= fetch();

	return tmp;
}

Word USimBE::read_word(Word offset)
{
	Word		tmp;

	tmp  = read(offset++) << 8;
	tmp |= read(offset);

	return tmp;
}

void USimBE::write_word(Word offset, Word val)
{
	write(offset++, (Byte)(val >> 8));
	write(offset, (Byte)val);
}

//----------------------------------------------------------------------------
// Word memory access routines for little-endian (Intel type)
//----------------------------------------------------------------------------

Word USimLE::fetch_word()
{
	Word		tmp;

	tmp  = fetch();
	tmp |= fetch() << 8;

	return tmp;
}

Word USimLE::read_word(Word offset)
{
	Word		tmp;

	tmp = read(offset++);
	tmp |= (read(offset) << 8);

	return tmp;
}

void USimLE::write_word(Word offset, Word val)
{
	write(offset++, (Byte)val);
	write(offset, (Byte)(val >> 8));
}
