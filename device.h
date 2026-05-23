//
//	device.h
//	(C) R.P.Bellis 2021 - 2025
//
//	vim: ts=8
//

#pragma once

#include <memory>
#include <vector>
#include <functional>
#include "typedefs.h"

/*
 * an abstract device that responds to CPU cycle ticks and might be reset
 */
class ActiveDevice {

public:
	virtual void		reset() = 0;
	virtual void		tick(uint8_t cycles) = 0;

public:
	using shared_ptr = std::shared_ptr<ActiveDevice>;

	virtual			~ActiveDevice() {};
};

/*
 * an abstract memory mapped device
 */
class MappedDevice {

public:
	virtual Byte		read(Word offset) = 0;
	virtual void		write(Word offset, Byte val) = 0;

public:
	using shared_ptr = std::shared_ptr<MappedDevice>;

	virtual			~MappedDevice() {};
};

/*
 * an abstract class combining the above two features
 */
class ActiveMappedDevice : virtual public ActiveDevice, virtual public MappedDevice {
public:
	using shared_ptr = std::shared_ptr<ActiveMappedDevice>;

	virtual			~ActiveMappedDevice() {};
};

/*
 * a container for mapping from memory locations to MappedDevices.
 *
 * Two matching styles are supported:
 *   - mask-mode: hit when (offset & mask) == base. Cheap and natural
 *     for power-of-2-aligned devices (vector ROM, RAM fallback, etc.).
 *     `size` is 0 when this mode is in use.
 *   - range-mode: hit when offset ∈ [base, base+size). Used by devices
 *     whose address span isn't power-of-2 aligned — e.g. pico-thing's
 *     console ACIA at $FFC3-$FFC4. `size` is nonzero in this mode and
 *     `mask` is unused.
 *
 * USim::read / USim::write check `size > 0` to pick the mode. The two
 * fields are mutually exclusive.
 */
struct MappedDeviceEntry {
	MappedDevice::shared_ptr	device;
	Word				base;
	Word				mask;
	Word				size;
};

/*
 * a container for ActiveDevices {
 */
struct ActiveDeviceEntry {
	ActiveDevice::shared_ptr	device;
};

typedef std::vector<ActiveDeviceEntry> ActiveDevList;
typedef std::vector<MappedDeviceEntry> MappedDevList;

/*
 * template to resolve smart point ambiguity issues
 * see https://stackoverflow.com/questions/66032442/
 */
template<int n> struct rank : rank<n - 1> {};
template<>      struct rank<0> {};
