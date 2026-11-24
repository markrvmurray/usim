//
//	test_ptylink.cpp
//	usim09pt's --console-pty=PATH and --aux-pty=PATH: a symlink to the
//	run's pty at a fixed path, carrying the guest's console; the
//	emulator's own terminal left as it was; the link gone when the run
//	ends, by its own exit or by a signal; another running emulator's
//	path refused, a dead run's replaced, and anything that is not a
//	link never touched. And PtySerial itself: a signal arriving while
//	it looks for input is not input.
//
//	vim: ts=8 sw=8 noet:
//

// posix_openpt, mkdtemp, usleep: beyond the POSIX.1 of 1988 the build
// asks for (macOS also wants its own switch for them)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "ptyserial.h"

static int failures = 0;

static void expect(const char* what, bool ok, const std::string& got = "")
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n%s\n", what, got.c_str());
		++failures;
	}
}

static char dir[] = "/tmp/test_ptylink_XXXXXX";

static std::string at(const char* name)
{
	return std::string(dir) + "/" + name;
}

// An S-record file holding code at $E000 and a reset vector to it
static std::string program(const char* name, const std::vector<unsigned char>& code)
{
	auto record = [](FILE* f, unsigned addr, const unsigned char* data, unsigned n) {
		unsigned sum = n + 3 + (addr >> 8) + (addr & 0xff);
		fprintf(f, "S1%02X%04X", n + 3, addr);
		for (unsigned i = 0; i < n; i++) {
			fprintf(f, "%02X", data[i]);
			sum += data[i];
		}
		fprintf(f, "%02X\n", ~sum & 0xff);
	};
	std::string path = at(name);
	FILE* f = fopen(path.c_str(), "w");
	for (unsigned i = 0; i < code.size(); i += 16)
		record(f, 0xE000 + i, code.data() + i,
		       (unsigned)std::min<size_t>(16, code.size() - i));
	const unsigned char vec[] = { 0xE0, 0x00 };
	record(f, 0xFFFE, vec, 2);
	fprintf(f, "S9030000FC\n");
	fclose(f);
	return path;
}

// A terminal for the emulator's stdin: the slave of a pty of our own,
// in its ordinary (canonical, echoing) mode
struct Tty {
	int	master = -1;
	int	slave = -1;

	Tty()
	{
		master = posix_openpt(O_RDWR | O_NOCTTY);
		if (master < 0 || grantpt(master) || unlockpt(master) ||
		    (slave = open(ptsname(master), O_RDWR | O_NOCTTY)) < 0) {
			perror("tty");
			exit(EXIT_FAILURE);
		}
		struct termios t;
		tcgetattr(slave, &t);
		t.c_lflag |= ICANON | ECHO;
		tcsetattr(slave, TCSANOW, &t);
	}
	~Tty()
	{
		close(slave);
		close(master);
	}
	bool ordinary() const
	{
		struct termios t;
		return tcgetattr(slave, &t) == 0 &&
		       (t.c_lflag & (ICANON | ECHO)) == (ICANON | ECHO);
	}
};

// usim09pt, started; its stderr goes to a file
static pid_t start(const std::vector<std::string>& args, int stdin_fd = -1)
{
	const std::string err = at("stderr");
	pid_t pid = fork();
	if (pid == 0) {
		if (stdin_fd >= 0)
			dup2(stdin_fd, 0);
		else
			freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen(err.c_str(), "w", stderr);
		std::vector<std::string> argv = { "./usim09pt",
			"-f", at("fram"), "--rtc-time=2026-01-01T00:00:00" };
		argv.insert(argv.end(), args.begin(), args.end());
		std::vector<char*> cargv;
		for (auto& a : argv)
			cargv.push_back(a.data());
		cargv.push_back(nullptr);
		execv(cargv[0], cargv.data());
		_exit(127);
	}
	return pid;
}

// Its exit status (128 + N for signal N), or -1 if it ran past 20s
static int finish(pid_t pid)
{
	int status = 0;
	for (int waited = 0; waitpid(pid, &status, WNOHANG) == 0; usleep(50000)) {
		if (++waited > 20 * 20) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return -1;
		}
	}
	return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
}

static std::string errors()
{
	std::string out;
	if (FILE* f = fopen(at("stderr").c_str(), "r")) {
		char buf[512];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0)
			out.append(buf, n);
		fclose(f);
	}
	return out;
}

static std::string target(const std::string& link)
{
	char buf[PATH_MAX];
	ssize_t n = readlink(link.c_str(), buf, sizeof buf - 1);
	return n < 0 ? "" : std::string(buf, (size_t)n);
}

static bool exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}

// Waits up to 10s for path to be a link to a /dev node
static bool wait_for_link(const std::string& path)
{
	for (int i = 0; i < 200; i++, usleep(50000))
		if (target(path).rfind("/dev/", 0) == 0)
			return true;
	return false;
}

// The guest's console bytes, from the link, waiting up to 5s for n
static std::string read_console(const std::string& link, size_t n)
{
	int fd = open(link.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return "(cannot open " + link + ": " + strerror(errno) + ")";
	std::string got;
	for (int i = 0; i < 100 && got.size() < n; i++) {
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 50) > 0) {
			char buf[64];
			ssize_t r = read(fd, buf, sizeof buf);
			if (r > 0)
				got.append(buf, (size_t)r);
		}
	}
	close(fd);
	return got;
}

static void on_alarm(int) {}

// Signals landing while PtySerial looks for input, with nothing to
// read: none of them may be taken for input. One that was would block
// the emulator in a read for a byte that never comes, so the looking
// is done in a child, and a child still at it after 10s has blocked.
static void test_poll_under_signals()
{
	pid_t pid = fork();
	if (pid == 0) {
		freopen("/dev/null", "w", stderr);	// its startup banner
		PtySerial pty("test");
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_alarm;
		sa.sa_flags = SA_RESTART;	// as a handler may: a read restarts
		sigaction(SIGALRM, &sa, nullptr);
		struct itimerval every = { { 0, 200 }, { 0, 200 } };
		setitimer(ITIMER_REAL, &every, nullptr);
		for (int i = 0; i < 20000; i++)
			if (pty.poll_read())
				_exit(2);	// a byte from nowhere
		_exit(0);
	}
	int status = 0;
	for (int waited = 0; waitpid(pid, &status, WNOHANG) == 0; usleep(50000)) {
		if (++waited > 10 * 20) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			expect("a signal is not input (it blocked in a read)", false);
			return;
		}
	}
	expect("a signal is not input", WIFEXITED(status) && WEXITSTATUS(status) == 0,
		std::to_string(status));
}

// Signals landing while PtySerial waits for room to write: none may
// cost a byte. A slow reader keeps the pty full, so every write waits.
static void test_write_under_signals()
{
	pid_t pid = fork();
	if (pid == 0) {
		freopen("/dev/null", "w", stderr);
		PtySerial pty("test");
		int fd = open(pty.name().c_str(), O_RDWR | O_NOCTTY);
		const int N = 3000;
		int got = 0;
		std::thread reader([&]() {
			char c;
			while (got < N) {
				struct pollfd p = { fd, POLLIN, 0 };
				if (poll(&p, 1, 2000) <= 0)
					break;		// nothing for 2s: the rest was lost
				if (read(fd, &c, 1) == 1)
					got++;
				usleep(300);
			}
		});
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_alarm;
		sa.sa_flags = SA_RESTART;
		sigaction(SIGALRM, &sa, nullptr);
		struct itimerval every = { { 0, 200 }, { 0, 200 } };
		setitimer(ITIMER_REAL, &every, nullptr);
		for (int i = 0; i < N; i++)
			pty.write((Byte)'x');
		reader.join();
		_exit(got == N ? 0 : 3);
	}
	int status = 0;
	for (int waited = 0; waitpid(pid, &status, WNOHANG) == 0; usleep(50000)) {
		if (++waited > 30 * 20) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			break;
		}
	}
	expect("a signal costs no byte written", WIFEXITED(status) && WEXITSTATUS(status) == 0,
		std::to_string(status));
}

int main()
{
	test_poll_under_signals();
	test_write_under_signals();
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return EXIT_FAILURE;
	}
	const std::string con = at("console"), aux = at("aux");

	// "hi" on the console, each byte when the ACIA can take it, then spin
	const std::string hello = program("hello.s19", {
		0x8E, 0xE0, 0x15,		// $E000      ldx  #msg
		0xF6, 0xFF, 0xC4,		// $E003 loop ldb  $FFC4
		0xC5, 0x02,			//            bitb #TDRE
		0x27, 0xF9,			//            beq  loop
		0xA6, 0x80,			//            lda  ,x+
		0x27, 0x05,			//            beq  done
		0xB7, 0xFF, 0xC5,		//            sta  $FFC5
		0x20, 0xF0,			//            bra  loop
		0x20, 0xFE,			// $E013 done bra  *
		'h', 'i', 0,			// $E015 msg
	});

	{
		Tty tty;
		pid_t pid = start({ "--console-pty=" + con, "--aux-pty=" + aux, hello }, tty.slave);
		expect("the console link appears", wait_for_link(con), errors());
		expect("the aux link appears", wait_for_link(aux), errors());
		expect("they are different ptys", target(con) != target(aux),
			target(con) + " " + target(aux));
		expect("the guest's console is there", read_console(con, 2) == "hi", errors());
		expect("the emulator's own terminal is left as it was", tty.ordinary());

		// a second run wanting the same path is refused, and the
		// first's link stays as it was
		const std::string first = target(con);
		pid_t again = start({ "--console-pty=" + con, hello });
		int rc = finish(again);
		expect("a running emulator's path is refused", rc == 1 &&
			errors().find("belongs to another running emulator") != std::string::npos,
			std::to_string(rc) + "\n" + errors());
		expect("and its link is untouched", target(con) == first, target(con));

		kill(pid, SIGTERM);
		rc = finish(pid);
		expect("SIGTERM still ends it by SIGTERM", rc == 128 + SIGTERM, std::to_string(rc));
		expect("its links go with it", !exists(con) && !exists(aux) &&
			!exists(con + ".lock") && !exists(aux + ".lock"));
	}

	// a dead run's link, with its lock file still there but not held
	symlink("/dev/no-such-pty", con.c_str());
	close(open((con + ".lock").c_str(), O_CREAT | O_WRONLY, 0644));
	pid_t pid = start({ "--console-pty=" + con, "--timeout=2000", hello });
	int rc = finish(pid);
	expect("an ordinary end", rc == 124, std::to_string(rc) + "\n" + errors());
	expect("the dead run's link was replaced",
		errors().find(con + " -> /dev/") != std::string::npos, errors());
	expect("and removed at the end", !exists(con) && !exists(con + ".lock"));

	// something that is not a link is not ours to replace
	close(open(con.c_str(), O_CREAT | O_WRONLY, 0644));
	pid = start({ "--console-pty=" + con, "--timeout=2000", hello });
	rc = finish(pid);
	expect("a file at the path is refused", rc == 1 &&
		errors().find("is not a link") != std::string::npos,
		std::to_string(rc) + "\n" + errors());
	struct stat st;
	expect("and left alone", lstat(con.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
		!exists(con + ".lock"));

	std::string cleanup = std::string("rm -rf ") + dir;
	(void)!system(cleanup.c_str());

	if (failures) {
		printf("test_ptylink: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_ptylink: PASS\n");
	return EXIT_SUCCESS;
}
