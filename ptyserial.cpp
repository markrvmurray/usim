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
#include <csignal>
#include <cstring>
#include <climits>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>

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
	unpublish();
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
	// Only a select() that says so means a byte is there: one a signal
	// cut short (EINTR) leaves the set as we filled it, and the read
	// that followed would wait, blocking, for a byte that may never come.
	fd_set		fds;
	struct timeval	tv = { 0L, 10L };
	FD_ZERO(&fds);
	FD_SET(master_fd, &fds);
	if (select(master_fd + 1, &fds, NULL, NULL, &tv) <= 0 ||
	    !FD_ISSET(master_fd, &fds))
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
	// A signal cutting the wait short is not a full buffer: wait again.
	fd_set		fds;
	int		ready;
	do {
		struct timeval	tv = { 0L, 100000L };	// 100ms ceiling
		FD_ZERO(&fds);
		FD_SET(master_fd, &fds);
		ready = select(master_fd + 1, NULL, &fds, NULL, &tv);
	} while (ready < 0 && errno == EINTR);
	if (ready <= 0)
		return;				// no room — drop the byte

	ssize_t n;
	do {
		n = ::write(master_fd, &c, 1);
	} while (n < 0 && errno == EINTR);
}

//------------------------------------------------------------------------
// A fixed path for the slave
//------------------------------------------------------------------------

// The published links, for removal when a signal ends the emulator: a
// signal handler can use only what is already in place, so each is kept
// as plain strings, in slots it can read without taking a lock.
struct PublishedLink {
	char		link[PATH_MAX];		// "" when the slot is free
	char		target[PATH_MAX];
	char		lock[PATH_MAX];
};
static PublishedLink published[4];

// Removes a link only while it still points at our pty, then its lock
// file. Only async-signal-safe calls: it runs from a signal handler.
static void remove_link(PublishedLink& p)
{
	if (!p.link[0])
		return;
	char now[PATH_MAX];
	ssize_t n = readlink(p.link, now, sizeof now - 1);
	if (n >= 0) {
		now[n] = 0;
		if (strcmp(now, p.target) == 0)
			unlink(p.link);
	}
	unlink(p.lock);
	p.link[0] = 0;
}

static void remove_all_links()
{
	for (auto& p : published)
		remove_link(p);
}

// A signal that ends the emulator: take the links with it, then die of
// the signal as before.
static void links_on_signal(int sig)
{
	remove_all_links();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void watch_for_exit()
{
	static bool installed = false;
	if (installed)
		return;
	installed = true;
	atexit(remove_all_links);
	for (int sig : { SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGABRT, SIGFPE }) {
		struct sigaction sa, old;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = links_on_signal;
		sigemptyset(&sa.sa_mask);
		// a signal the emulator was told to ignore stays ignored
		if (sigaction(sig, nullptr, &old) == 0 && old.sa_handler == SIG_IGN)
			continue;
		sigaction(sig, &sa, nullptr);
	}
}

bool PtySerial::publish(const std::string& path)
{
	if (slave_name.empty()) {
		fprintf(stderr, "ptyserial: no pty to link %s to\n", path.c_str());
		return false;
	}
	if (path.size() + 6 >= PATH_MAX) {
		fprintf(stderr, "ptyserial: %s: path too long\n", path.c_str());
		return false;
	}
	PublishedLink* slot = nullptr;
	for (auto& p : published)
		if (!p.link[0]) {
			slot = &p;
			break;
		}
	if (!slot) {
		fprintf(stderr, "ptyserial: too many fixed pty paths\n");
		return false;
	}

	// The lock. A lock file can be removed between our opening it and
	// locking it, so the lock counts only if it is still on the file
	// the path names; otherwise try again on the new one.
	const std::string lock = path + ".lock";
	int fd = -1;
	for (;;) {
		fd = open(lock.c_str(), O_RDWR | O_CREAT, 0644);
		if (fd < 0) {
			perror(lock.c_str());
			return false;
		}
		if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
			if (errno == EWOULDBLOCK)
				fprintf(stderr, "ptyserial: %s belongs to another "
					"running emulator\n", path.c_str());
			else
				perror(lock.c_str());
			close(fd);
			return false;
		}
		struct stat held, named;
		if (fstat(fd, &held) == 0 && stat(lock.c_str(), &named) == 0 &&
		    held.st_dev == named.st_dev && held.st_ino == named.st_ino)
			break;
		close(fd);
	}

	// Ours now. Anything at the path is a link a dead run left, unless
	// it is not a link at all, which is not ours to replace.
	struct stat st;
	if (lstat(path.c_str(), &st) == 0 && !S_ISLNK(st.st_mode)) {
		fprintf(stderr, "ptyserial: %s exists and is not a link; "
			"not replacing it\n", path.c_str());
		unlink(lock.c_str());
		close(fd);
		return false;
	}
	// made beside it, then renamed over it, so the path is never missing
	// or half made while a client looks
	const std::string tmp = path + ".new";
	unlink(tmp.c_str());
	if (symlink(slave_name.c_str(), tmp.c_str()) != 0 ||
	    rename(tmp.c_str(), path.c_str()) != 0) {
		perror(path.c_str());
		unlink(tmp.c_str());
		unlink(lock.c_str());
		close(fd);
		return false;
	}

	strcpy(slot->target, slave_name.c_str());
	strcpy(slot->lock, lock.c_str());
	strcpy(slot->link, path.c_str());
	link_path = path;
	lock_fd = fd;
	watch_for_exit();
	fprintf(stderr, "ptyserial: %s -> %s\n", path.c_str(), slave_name.c_str());
	return true;
}

void PtySerial::unpublish()
{
	if (link_path.empty())
		return;
	for (auto& p : published)
		if (link_path == p.link)
			remove_link(p);
	link_path.clear();
	close(lock_fd);		// after the lock file has gone
	lock_fd = -1;
}
