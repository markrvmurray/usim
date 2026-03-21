//
//	batchterm.h
//	Batch-mode mc6850_impl for non-interactive use
//
//	vim: ts=8
//

#pragma once

#include <cstdio>
#include "mc6850.h"

class BatchTerminal : virtual public mc6850_impl {

public:
	virtual bool		poll_read();
	virtual Byte		read();
	virtual void		write(Byte);

public:
				BatchTerminal() = default;
	virtual			~BatchTerminal() = default;

};
