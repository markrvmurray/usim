//
//	ptyserial.h
//	PTY-backed mc6850_impl for the auxiliary ACIA (usim09pt)
//
//	Allocates a host pseudo-terminal and exposes it as a serial
//	backend for an MC6850. The emulator holds the PTY master; the
//	slave device node (e.g. /dev/ttysNNN on macOS) is printed to
//	stderr at startup so an external program — DriveWire, a terminal,
//	a serial tool — can attach to it. The link is configured raw
//	(8-bit, no echo, no CR/LF translation, no signals) so a binary
//	protocol passes through untouched.
//
//	The slave's name changes from run to run. publish() also gives it
//	a fixed one: a symlink at a path of the caller's choosing, so a
//	client can be pointed at it once and find each run's pty there.
//	The link is owned through a lock on PATH.lock, held for the run:
//	a link another live emulator owns is refused, never taken over,
//	and one left by a run that died is replaced. Both are removed when
//	the emulator exits, also when a signal ends it.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <string>
#include "mc6850.h"

class PtySerial : virtual public mc6850_impl {

protected:
	int		master_fd = -1;	// PTY master held by the emulator
	int		slave_anchor = -1;	// one slave fd we keep open: pins
						// the pts and its raw termios, and
						// stops the master seeing EOF before
						// the real client attaches
	std::string	slave_name;	// /dev/ttysNNN — printed for the client

	// One-byte read-ahead, mirroring Terminal: poll_read() does the
	// actual host read and latches the byte; read() hands it back. This
	// guarantees RDRF is only ever raised for a byte that genuinely
	// arrived (no spurious bytes on a select() false-positive).
	Byte		pending = 0;
	bool		have_byte = false;

	void		configure_raw(int fd);

	std::string	link_path;	// publish()'s symlink, while it is ours
	int		lock_fd = -1;	// the flock on link_path + ".lock"
	void		unpublish();

public:
	virtual bool	poll_read() override;
	virtual Byte	read() override;
	virtual void	write(Byte) override;

	bool		ok() const { return master_fd >= 0; }
	const std::string& name() const { return slave_name; }

	// Make path a symlink to the slave, as described above. False,
	// with the reason on stderr, if it cannot be had.
	bool		publish(const std::string& path);

public:
	// label is only used in the startup banner (e.g. "DriveWire").
	explicit	PtySerial(const char* label = "aux");
	virtual		~PtySerial();
};
