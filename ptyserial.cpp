//
//	ptyserial.cpp
//	PTY-backed mc6850_impl for the auxiliary ACIA (usim09pt)
//
//	vim: ts=8 sw=8 noet:
//

// posix_openpt/grantpt/unlockpt/ptsname are XSI; the Makefile compiles
// with -D_POSIX_SOURCE, which on Darwin hides them. Re-open the full
// Darwin namespace here, and ask for the XSI extensions on glibc, so
// these declarations are visible without disturbing the rest of the
// build.
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600
#endif

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

#include "ptyserial.h"

PtySerial::PtySerial(const char* label)
{
	master_fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (master_fd < 0) {
		perror("ptyserial: posix_openpt");
		return;
	}
	if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
		perror("ptyserial: grantpt/unlockpt");
		close(master_fd);
		master_fd = -1;
		return;
	}

	const char* sn = ptsname(master_fd);
	if (sn) slave_name = sn;

	// Open the slave once and keep it: this both lets us force raw mode
	// on the line discipline and anchors the pts so the master doesn't
	// read EOF/EIO in the window before the real client connects.
	if (!slave_name.empty()) {
		slave_anchor = open(slave_name.c_str(), O_RDWR | O_NOCTTY);
		if (slave_anchor >= 0)
			configure_raw(slave_anchor);
	}

	fprintf(stderr,
		"ptyserial: %s ACIA attached to %s\n",
		label ? label : "aux",
		slave_name.empty() ? "(unknown pty)" : slave_name.c_str());
}

PtySerial::~PtySerial()
{
	if (slave_anchor >= 0) close(slave_anchor);
	if (master_fd >= 0)    close(master_fd);
}

//
// Force a raw 8-bit link: no input mapping (CR/LF, strip, XON/XOFF), no
// output post-processing, no echo / canonical / signal handling. This is
// what a binary serial protocol such as DriveWire needs. Equivalent to
// cfmakeraw(), spelled out so it compiles identically on Darwin and glibc
// without relying on the macro being exposed under the build's feature
// macros.
//
void PtySerial::configure_raw(int fd)
{
	struct termios t;
	if (tcgetattr(fd, &t) != 0)
		return;

	t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
				 INLCR | IGNCR | ICRNL | IXON);
	t.c_oflag &= ~(tcflag_t)OPOST;
	t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
	t.c_cflag |=  (tcflag_t)CS8;
	t.c_cc[VMIN]  = 1;
	t.c_cc[VTIME] = 0;

	(void)tcsetattr(fd, TCSANOW, &t);
}

bool PtySerial::poll_read()
{
	if (have_byte)
		return true;
	if (master_fd < 0)
		return false;

	// 10us select timeout, matching Terminal: keeps an idle simulation
	// from spinning the host CPU while still being effectively instant.
	fd_set		fds;
	struct timeval	tv = { 0L, 10L };
	FD_ZERO(&fds);
	FD_SET(master_fd, &fds);
	(void)select(master_fd + 1, &fds, NULL, NULL, &tv);
	if (!FD_ISSET(master_fd, &fds))
		return false;

	unsigned char	c;
	ssize_t		n = ::read(master_fd, &c, 1);
	if (n == 1) {
		pending = (Byte)c;
		have_byte = true;
		return true;
	}
	// n == 0 (EOF) or n < 0 (EAGAIN/EINTR/EIO): no byte to deliver. The
	// anchored slave fd normally keeps EOF/EIO from happening at all.
	return false;
}

Byte PtySerial::read()
{
	have_byte = false;
	return pending;
}

void PtySerial::write(Byte ch)
{
	if (master_fd < 0)
		return;

	unsigned char	c = (unsigned char)ch;

	// Wait briefly for room in the master->slave buffer, then write.
	// A connected client drains promptly, so this returns at once in the
	// normal case. The bounded wait stops a missing/stalled client from
	// hanging the whole emulator: if no space frees up, the byte is
	// dropped (a real UART would have overrun its shift register too).
	fd_set		fds;
	struct timeval	tv = { 0L, 100000L };	// 100ms ceiling
	FD_ZERO(&fds);
	FD_SET(master_fd, &fds);
	if (select(master_fd + 1, NULL, &fds, NULL, &tv) <= 0)
		return;				// no room — drop the byte

	ssize_t n;
	do {
		n = ::write(master_fd, &c, 1);
	} while (n < 0 && errno == EINTR);
}
