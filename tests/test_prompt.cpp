//
//	test_prompt.cpp
//	usim09pt --prompt, driven through its stdin and stdout as a person
//	at its terminal would: the guest held at reset until "run", the
//	console on its pty meanwhile silent; stop, step, breakpoints,
//	registers and memory; Ctrl-C; the machine's own commands beside
//	the prompt's; the end of input; an invalid instruction; the option
//	it cannot go without; and a debugger, taking the CPU from the
//	prompt and giving it back.
//
//	vim: ts=8 sw=8 noet:
//

// mkdtemp, usleep: beyond the POSIX.1 of 1988 the build asks for (macOS
// also wants its own switch for them)
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

static void expect(const char* what, bool ok, const std::string& got = "")
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n%s\n", what, got.c_str());
		++failures;
	}
}

static bool has(const std::string& s, const std::string& part)
{
	return s.find(part) != std::string::npos;
}

static char dir[] = "/tmp/test_prompt_XXXXXX";

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

// usim09pt with pipes to its stdin and from its stdout; stderr to a file
struct Emulator {
	pid_t		pid = -1;
	int		to = -1;	// its stdin
	int		from = -1;	// its stdout
	std::string	seen;		// its stdout so far, not yet consumed

	explicit Emulator(const std::vector<std::string>& args)
	{
		int in[2], out[2];
		if (pipe(in) || pipe(out)) {
			perror("pipe");
			exit(EXIT_FAILURE);
		}
		const std::string err = at("stderr");
		unlink(err.c_str());	// nothing of the last run's
		pid = fork();
		if (pid == 0) {
			dup2(in[0], 0);
			dup2(out[1], 1);
			close(in[0]); close(in[1]); close(out[0]); close(out[1]);
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
		close(in[0]);
		close(out[1]);
		to = in[1];
		from = out[0];
	}
	~Emulator()
	{
		if (to >= 0) close(to);
		if (from >= 0) close(from);
		if (pid > 0) {
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
		}
	}

	void send(const std::string& line)
	{
		std::string l = line + "\n";
		(void)!write(to, l.data(), l.size());
	}
	void close_input()
	{
		close(to);
		to = -1;
	}

	// Its output up to and including the next `until`, waiting up to
	// 10s; what it had, marked, if that never came
	std::string upto(const std::string& until)
	{
		for (int i = 0; i < 200; i++) {
			size_t p = seen.find(until);
			if (p != std::string::npos) {
				std::string got = seen.substr(0, p + until.size());
				seen.erase(0, p + until.size());
				return got;
			}
			struct pollfd pf = { from, POLLIN, 0 };
			if (poll(&pf, 1, 50) > 0) {
				char buf[512];
				ssize_t r = read(from, buf, sizeof buf);
				if (r > 0)
					seen.append(buf, (size_t)r);
				else if (r == 0)
					break;
			}
		}
		std::string got = seen + "(never saw \"" + until + "\")";
		seen.clear();
		return got;
	}
	// A command's reply: its output up to the next prompt
	std::string cmd(const std::string& line)
	{
		send(line);
		return upto("usim09pt> ");
	}

	// Its exit status (128 + N for signal N), or -1 if it ran past 20s
	int finish()
	{
		int status = 0;
		for (int waited = 0; waitpid(pid, &status, WNOHANG) == 0; usleep(50000)) {
			if (++waited > 20 * 20) {
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				pid = -1;
				return -1;
			}
		}
		pid = -1;
		return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
	}
};

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

// What the guest has sent its console, waiting up to ms for n bytes
static std::string console(int fd, size_t n, int ms)
{
	std::string got;
	for (int i = 0; i < ms / 10 && got.size() < n; i++) {
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 10) > 0) {
			char buf[64];
			ssize_t r = read(fd, buf, sizeof buf);
			if (r > 0)
				got.append(buf, (size_t)r);
		}
	}
	return got;
}

// The port a debugger is awaited on, from the emulator's stderr,
// waiting up to 10s for it to say
static int debugger_port()
{
	for (int i = 0; i < 200; i++, usleep(50000)) {
		const std::string err = errors();
		size_t p = err.find("debugger on localhost:");
		if (p != std::string::npos)
			return atoi(err.c_str() + p + strlen("debugger on localhost:"));
	}
	return -1;
}

// A debugger, as far as these tests need one: the remote protocol by
// hand over TCP
struct Debugger {
	int		fd = -1;

	explicit Debugger(int port)
	{
		fd = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons((uint16_t)port);
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		if (port <= 0 || connect(fd, (sockaddr*)&a, sizeof a) != 0) {
			close(fd);
			fd = -1;
		}
	}
	~Debugger()
	{
		if (fd >= 0)
			close(fd);
	}
	// A packet's reply payload, or "<none>"
	std::string cmd(const std::string& payload)
	{
		unsigned sum = 0;
		for (unsigned char c : payload)
			sum += c;
		char tail[8];
		snprintf(tail, sizeof tail, "#%02x", sum & 0xff);
		const std::string pkt = "$" + payload + tail;
		(void)!write(fd, pkt.data(), pkt.size());
		std::string in;
		for (int i = 0; i < 200; i++) {
			size_t d = in.find('$'), h = in.find('#', d == std::string::npos ? 0 : d);
			if (d != std::string::npos && h != std::string::npos && in.size() >= h + 3)
				return in.substr(d + 1, h - d - 1);
			struct pollfd p = { fd, POLLIN, 0 };
			if (poll(&p, 1, 50) > 0) {
				char buf[256];
				ssize_t r = read(fd, buf, sizeof buf);
				if (r <= 0)
					break;
				in.append(buf, (size_t)r);
			}
		}
		return "<none>";
	}
};

int main()
{
	signal(SIGPIPE, SIG_IGN);	// an emulator that has gone is a failure, not ours
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return EXIT_FAILURE;
	}
	const std::string con = at("console");

	// "hi" on the console, each byte when the ACIA can take it, then spin
	const std::string hello = program("hello.s19", {
		0x8E, 0xE0, 0x15,		// $E000      ldx  #msg
		0xF6, 0xFF, 0xC4,		// $E003 loop ldb  $FFC4
		0xC5, 0x02,			//            bitb #TDRE
		0x27, 0xF9,			//            beq  loop
		0xA6, 0x80,			//            lda  ,x+
		0x27, 0x05,			//            beq  done
		0xB7, 0xFF, 0xC5,		// $E00E      sta  $FFC5
		0x20, 0xF0,			// $E011      bra  loop
		0x20, 0xFE,			// $E013 done bra  *
		'h', 'i', 0,			// $E015 msg
	});
	std::string out;

	{
		Emulator e({ "--console-pty=" + con, "--prompt", "--timeout=50000000", hello });
		out = e.upto("usim09pt> ");
		expect("held at reset", has(out, "held at reset at $E000") &&
			has(out, "PC:E000"), out + errors());
		int fd = open(con.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
		expect("the console's link", fd >= 0, errors());
		expect("nothing runs while held", console(fd, 1, 500).empty());
		out = e.cmd("regs");
		expect("still at reset after a wait", has(out, "PC:E000"), out);

		out = e.cmd("help");
		expect("help: the prompt's commands and the machine's",
			has(out, "run   ") && has(out, "step [N]") && has(out, "mem ADDR") &&
			has(out, "trace on|off") && has(out, "dat [N]") && has(out, "phys ADDR"), out);
		expect("one help line", out.find("help  ") == out.rfind("help  "), out);
		out = e.cmd("mem E015 3");
		expect("mem", has(out, "E015: 68 69 00") && has(out, "hi."), out);
		out = e.cmd("mem $E015 2");
		expect("mem, $ and a length", has(out, "E015: 68 69 ") && !has(out, "68 69 00"), out);
		out = e.cmd("mem zz");
		expect("mem, wrongly", has(out, "usage: mem ADDR [LEN]"), out);
		out = e.cmd("dat");
		expect("the machine's commands", has(out, "task 0:") && has(out, "($E000 fixed)"), out);
		out = e.cmd("frob");
		expect("unknown", has(out, "unknown command: frob (try \"help\")"), out);

		// a breakpoint, a step, and on
		e.cmd("break E00E");
		out = e.cmd("break");
		expect("break lists", has(out, "$E00E"), out);
		out = e.cmd("run");
		expect("stops at a breakpoint", has(out, "breakpoint at $E00E") &&
			has(out, "A:68"), out);
		out = e.cmd("run");
		expect("a breakpoint where it stands is passed, the next one met",
			has(out, "breakpoint at $E00E") && has(out, "A:69"), out);
		out = e.cmd("step");
		expect("one step", has(out, "stepped at $E011"), out);
		out = e.cmd("step 3");
		expect("three steps", has(out, "stepped at $E008"), out);
		out = e.cmd("step 0");
		expect("step, wrongly", has(out, "usage: step [N]"), out);
		out = e.cmd("delete E00E");
		expect("delete", !has(out, "no breakpoint"), out);
		out = e.cmd("delete E00E");
		expect("delete, again", has(out, "no breakpoint at $E00E"), out);

		// running: the console, and commands between instructions
		e.send("run");
		expect("the guest's console", console(fd, 2, 5000) == "hi");
		// no prompt while it runs, as in gdb: each reply is its own
		e.send("regs");
		out = e.upto("DP:00\n");
		expect("regs while running", has(out, "PC:E01") && !has(out, "usim09pt> "), out);
		e.send("run");
		out = e.upto("running\n");
		expect("run while running", !has(out, "usim09pt> ") && !has(out, "never saw"), out);
		e.send("step");
		out = e.upto("running: stop it first\n");
		expect("step while running", !has(out, "never saw"), out);
		out = e.cmd("stop");
		expect("stop", has(out, "stopped at $E013"), out);
		out = e.cmd("stop");
		expect("stop, stopped", has(out, "not running"), out);

		// Ctrl-C
		e.send("run");
		usleep(200000);
		kill(e.pid, SIGINT);
		out = e.upto("usim09pt> ");
		expect("Ctrl-C stops it", has(out, "interrupted at $E013"), out);
		kill(e.pid, SIGINT);
		out = e.upto("usim09pt> ");
		expect("Ctrl-C at the prompt only gives a new one", true, out);
		out = e.cmd("regs");
		expect("and leaves it there", has(out, "PC:E013"), out);

		out = e.cmd("reset");
		expect("reset", has(out, "PC:E000"), out);

		e.send("quit");
		int rc = e.finish();
		expect("quit", rc == 0, std::to_string(rc) + "\n" + errors());
		struct stat st;
		expect("and the link goes", lstat(con.c_str(), &st) != 0);
		close(fd);
	}

	{
		// the end of input stops nothing that runs: the run ends its own way
		Emulator e({ "--console-pty=" + con, "--prompt", "--timeout=5000", hello });
		e.send("run");
		e.close_input();
		int rc = e.finish();
		expect("a script's run ends by itself", rc == 124, std::to_string(rc) + "\n" + errors());
	}
	{
		// at the prompt it is quit
		Emulator e({ "--console-pty=" + con, "--prompt", "--timeout=5000", hello });
		e.close_input();
		int rc = e.finish();
		expect("the end of input at the prompt quits", rc == 0 &&
			!has(errors(), "timeout"), std::to_string(rc) + "\n" + errors());
	}
	{
		// an invalid instruction: stopped for good, but open to look at
		const std::string invalid = program("invalid.s19", {
			0x10, 0xCE, 0xE8, 0x00,	// lds  #$E800
			0x14,			// no such instruction
		});
		Emulator e({ "--console-pty=" + con, "--prompt", invalid });
		e.upto("usim09pt> ");
		out = e.cmd("run");
		expect("an invalid instruction stops it", has(out, "invalid instruction at $E004") &&
			has(out, "S:E800"), out);
		out = e.cmd("run");
		expect("for good", has(out, "cannot go on past an invalid instruction"), out);
		out = e.cmd("reset");
		expect("reset too", has(out, "cannot go on past an invalid instruction"), out);
		out = e.cmd("mem E004 1");
		expect("but open to look at", has(out, "E004: 14"), out);
		e.send("quit");
		int rc = e.finish();
		expect("and it ends as a failure", rc == 1, std::to_string(rc));
		expect("with the post-mortem", has(errors(), "CRASH POST-MORTEM"), errors());
	}
	{
		Emulator e({ "--prompt", hello });
		int rc = e.finish();
		expect("--prompt needs the console off stdin", rc == 1 &&
			has(errors(), "--prompt needs the console off stdin"), errors());
	}
	{
		// "gdb": the debugger has the CPU until it detaches
		Emulator e({ "--console-pty=" + con, "--prompt", "--timeout=50000000", hello });
		e.upto("usim09pt> ");
		out = e.cmd("gdb x");
		expect("gdb, wrongly", has(out, "usage: gdb [PORT]"), out);
		e.send("gdb 0");
		out = e.upto("(Ctrl-C gives up)\n");
		expect("waits for a debugger", !has(out, "never saw"), out);
		Debugger d(debugger_port());
		expect("a debugger attaches", d.fd >= 0, errors());
		out = e.upto("gives it back here\n");
		expect("the prompt says so", has(out, "the debugger has the CPU"), out);
		std::string r = d.cmd("p6");
		expect("the debugger finds the CPU where the prompt left it", r == "e000", r);
		e.send("regs");		// read only once the debugger lets go
		r = d.cmd("D");
		expect("it detaches", r == "OK", r);
		out = e.upto("DP:00\n");
		expect("the prompt has the CPU back, running",
			has(out, "the debugger has let go: running") && has(out, "PC:E0"), out);
		int fd = open(con.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
		expect("the guest runs on", console(fd, 2, 5000) == "hi");
		close(fd);
		out = e.cmd("stop");
		expect("and stops when told", has(out, "stopped at $E013"), out);

		// Ctrl-C while waiting gives up
		e.send("gdb 0");
		e.upto("(Ctrl-C gives up)\n");
		usleep(200000);
		kill(e.pid, SIGINT);
		out = e.upto("usim09pt> ");
		expect("Ctrl-C gives up waiting", has(out, "no debugger attached"), out);
		out = e.cmd("regs");
		expect("and the prompt goes on", has(out, "PC:E013"), out);

		e.send("run");
		e.send("gdb 0");
		out = e.upto("stop it first\n");
		expect("not while running", has(out, "running: stop it first"), out);
		e.cmd("stop");

		// a kill ends the emulator, as it does under --gdb
		const size_t before = errors().size();
		e.send("gdb 0");
		e.upto("(Ctrl-C gives up)\n");
		int port = -1;
		for (int i = 0; i < 200 && port < 0; i++, usleep(50000)) {
			const std::string err = errors().substr(before);
			size_t p = err.find("debugger on localhost:");
			if (p != std::string::npos)
				port = atoi(err.c_str() + p + strlen("debugger on localhost:"));
		}
		Debugger k(port);
		expect("a second debugger attaches", k.fd >= 0, errors());
		k.cmd("k");
		int rc = e.finish();
		expect("its kill ends the emulator", rc == 1 &&
			has(errors(), "killed by the debugger"), std::to_string(rc) + "\n" + errors());
	}
	{
		// --gdb with --prompt: the debugger first, then the prompt
		Emulator e({ "--console-pty=" + con, "--prompt", "--gdb=0", hello });
		Debugger d(debugger_port());
		expect("--gdb with --prompt waits for the debugger", d.fd >= 0, errors());
		out = e.upto("gives it back here\n");
		expect("the debugger has it first", has(out, "the debugger has the CPU") &&
			!has(out, "held at reset"), out);
		std::string r = d.cmd("D");
		expect("it detaches", r == "OK", r);
		out = e.upto("Ctrl-C stops it)\n");
		expect("then the prompt has it, running", has(out, "the debugger has let go") &&
			!has(out, "held at reset"), out);
		out = e.cmd("stop");
		expect("stop", has(out, "stopped at $E0") &&	// wherever it had got to
			!has(out, "held at reset"), out);
		e.send("quit");
		int rc = e.finish();
		expect("quit", rc == 0, std::to_string(rc) + "\n" + errors());
	}

	std::string cleanup = std::string("rm -rf ") + dir;
	(void)!system(cleanup.c_str());

	if (failures) {
		printf("test_prompt: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_prompt: PASS\n");
	return EXIT_SUCCESS;
}
