//
//	prompt.cpp
//	See prompt.h.
//
//	vim: ts=8 sw=8 noet:
//

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <unistd.h>

#include "prompt.h"

// How many instructions run between looks at the terminal: often enough
// that a typed "stop" lands at once, seldom enough to cost nothing.
static const unsigned POLL_INTERVAL = 4096;

static volatile sig_atomic_t interrupted = 0;

static void on_interrupt(int)
{
	interrupted = 1;
}

void Prompt::catch_interrupts()
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_interrupt;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;		// no SA_RESTART: a waiting read wakes
	sigaction(SIGINT, &sa, nullptr);
}

// An address or a count, in hex, with an optional $ or 0x
static bool parse_hex(const std::string& s, unsigned long& v)
{
	std::string t = s;
	if (!t.empty() && t[0] == '$')
		t.erase(0, 1);
	else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
		t.erase(0, 2);
	if (t.empty())
		return false;
	char* end;
	v = strtoul(t.c_str(), &end, 16);
	return *end == 0;
}

static std::string hex(unsigned v, int digits)
{
	char buf[8];
	snprintf(buf, sizeof buf, "%0*X", digits, v);
	return buf;
}

Prompt::Prompt(mc6809& cpu, const Monitor& machine, int in_fd, FILE* out)
	: cpu(cpu), in_fd(in_fd), out(out), breakpoints(0x10000, false)
{
	read_memory = [&cpu](Word a) { return cpu.debug_read(a); };
	own.then(machine);

	own.add("run", "run               run the CPU from where it is\n",
		[this](const std::string&, std::string& o) {
			if (state == FAULTED)
				o += "the CPU cannot go on past an invalid instruction\n";
			else if (state == RUNNING && !counting)
				o += "running\n";
			else {
				state = RUNNING;
				counting = false;
			}
		});
	own.add("stop", "stop              stop the CPU (Ctrl-C does too)\n",
		[this](const std::string&, std::string& o) {
			if (state == RUNNING)
				stop("stopped");
			else
				o += "not running\n";
		});
	own.add("step", "step [N]          run N instructions (default 1), then stop\n",
		[this](const std::string& args, std::string& o) {
			unsigned long n = 1;
			if (!args.empty()) {
				char* end;
				n = strtoul(args.c_str(), &end, 10);
				if (*end || n == 0) {
					o += "usage: step [N] (N > 0, decimal)\n";
					return;
				}
			}
			if (state == FAULTED)
				o += "the CPU cannot go on past an invalid instruction\n";
			else if (state == RUNNING)
				o += "running: stop it first\n";
			else {
				state = RUNNING;
				counting = true;
				steps_left = n;
			}
		});
	own.add("regs", "regs              the CPU's registers\n",
		[this](const std::string&, std::string& o) { o += regs(); });
	own.add("mem", "mem ADDR [LEN]    memory from ADDR, LEN bytes (default 64; hex)\n",
		[this](const std::string& args, std::string& o) {
			unsigned long addr, len = 64;
			const size_t sp = args.find_first_of(" \t");
			const std::string a = args.substr(0, sp);
			const std::string l = sp == std::string::npos ? ""
				: args.substr(args.find_first_not_of(" \t", sp));
			if (!parse_hex(a, addr) || addr > 0xFFFF ||
			    (!l.empty() && !parse_hex(l, len))) {
				o += "usage: mem ADDR [LEN] (hex)\n";
				return;
			}
			len = std::min(len, 0x10000ul - addr);
			for (unsigned long i = 0; i < len; i += 16) {
				std::string text;
				o += hex((unsigned)(addr + i), 4) + ":";
				for (unsigned long j = i; j < i + 16; j++) {
					if (j < len) {
						Byte b = read_memory((Word)(addr + j));
						o += " " + hex(b, 2);
						text += b >= 0x20 && b < 0x7F ? (char)b : '.';
					} else {
						o += "   ";
					}
				}
				o += "  " + text + "\n";
			}
		});
	own.add("break", "break [ADDR]      stop before ADDR runs; alone, list them\n",
		[this](const std::string& args, std::string& o) {
			unsigned long a;
			if (args.empty()) {
				for (unsigned i = 0; i < 0x10000; i++)
					if (breakpoints[i])
						o += "$" + hex(i, 4) + "\n";
				if (o.empty())
					o += "no breakpoints\n";
			} else if (parse_hex(args, a) && a <= 0xFFFF) {
				breakpoints[a] = true;
			} else {
				o += "usage: break [ADDR] (hex)\n";
			}
		});
	own.add("delete", "delete [ADDR]     remove the breakpoint at ADDR; alone, all\n",
		[this](const std::string& args, std::string& o) {
			unsigned long a;
			if (args.empty())
				std::fill(breakpoints.begin(), breakpoints.end(), false);
			else if (parse_hex(args, a) && a <= 0xFFFF && breakpoints[a])
				breakpoints[a] = false;
			else if (parse_hex(args, a) && a <= 0xFFFF)
				o += "no breakpoint at $" + hex((unsigned)a, 4) + "\n";
			else
				o += "usage: delete [ADDR] (hex)\n";
		});
	own.add("reset", "reset             reset the CPU, as its RESET line does\n",
		[this](const std::string&, std::string& o) {
			if (state == RUNNING) {
				o += "running: stop it first\n";
				return;
			}
			if (state == FAULTED) {
				o += "the CPU cannot go on past an invalid instruction\n";
				return;
			}
			this->cpu.reset();
			state = STOPPED;
			o += regs();
		});
	own.add("quit", "quit              end the emulator\n",
		[this](const std::string&, std::string&) { state = QUIT; });
}

void Prompt::say(const std::string& text)
{
	fputs(text.c_str(), out);
	fflush(out);
}

std::string Prompt::regs() const
{
	char flags[] = "EFHINZVC";
	const Byte cc = cpu.get_cc();
	for (int i = 0; i < 8; i++)
		if (!(cc & (0x80 >> i)))
			flags[i] = '-';
	char buf[128];
	snprintf(buf, sizeof buf,
		"PC:%04X CC:%s S:%04X U:%04X A:%02X B:%02X X:%04X Y:%04X DP:%02X\n",
		cpu.get_pc(), flags, cpu.get_s(), cpu.get_u(), cpu.get_a(),
		cpu.get_b(), cpu.get_x(), cpu.get_y(), cpu.get_dp());
	return buf;
}

void Prompt::stop(const std::string& why)
{
	state = STOPPED;
	counting = false;
	say(why + " at $" + hex(cpu.get_pc(), 4) + "\n" + regs());
}

// Reads what has been typed into pending. With wait, waits (a while,
// keeping the machine alive) for something to come. False at the end
// of input.
bool Prompt::read_input(bool wait)
{
	if (at_eof)
		return false;
	for (;;) {
		struct pollfd p = { in_fd, POLLIN, 0 };
		int n = poll(&p, 1, wait ? 50 : 0);
		if (n > 0) {
			char buf[256];
			ssize_t r = read(in_fd, buf, sizeof buf);
			if (r > 0) {
				pending.append(buf, (size_t)r);
				return true;
			}
			if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
				at_eof = true;
				return false;
			}
		}
		if (!wait || interrupted)
			return true;
		if (idle)
			idle();
	}
}

void Prompt::run_line(const std::string& line)
{
	std::string o;
	if (!own.run(line, o)) {
		std::string word = line.substr(line.find_first_not_of(" \t"));
		word = word.substr(0, word.find_first_of(" \t\r\n"));
		o = "unknown command: " + word + " (try \"help\")\n";
	}
	say(o);
}

// The CPU is stopped: read and run commands until it is told to go on
// (true) or to quit (false)
bool Prompt::serve()
{
	say(name + "> ");
	for (;;) {
		size_t nl;
		while ((nl = pending.find('\n')) != std::string::npos) {
			const std::string line = pending.substr(0, nl);
			pending.erase(0, nl + 1);
			if (line.find_first_not_of(" \t\r") != std::string::npos)
				run_line(line);
			if (state == QUIT)
				return false;
			if (state == RUNNING)
				return true;
			say(name + "> ");
		}
		if (interrupted) {
			interrupted = 0;
			pending.clear();
			say("\n" + name + "> ");
		}
		if (!read_input(true)) {
			say("\n");
			state = QUIT;
			return false;
		}
	}
}

bool Prompt::before_insn()
{
	if (state == QUIT)
		return false;
	if (!started) {
		started = true;
		say("held at reset at $" + hex(cpu.get_pc(), 4) + "\n" + regs() +
		    "(\"run\" starts it; \"help\" lists the commands)\n");
	}
	// A resume happens inside the call that stopped, after these checks,
	// so the instruction it stopped at runs: a breakpoint there is passed.
	if (state == RUNNING) {
		if (interrupted) {
			interrupted = 0;
			stop("interrupted");
		} else if (counting && steps_left == 0) {
			stop("stepped");
		} else if (breakpoints[cpu.get_pc()]) {
			stop("breakpoint");
		} else {
			if (counting)
				steps_left--;
			// typed lines, between instructions
			if (poll_countdown-- == 0) {
				poll_countdown = POLL_INTERVAL;
				if (read_input(false)) {
					size_t nl;
					while (state == RUNNING &&
					       (nl = pending.find('\n')) != std::string::npos) {
						const std::string line = pending.substr(0, nl);
						pending.erase(0, nl + 1);
						if (line.find_first_not_of(" \t\r") != std::string::npos)
							run_line(line);
					}
					if (state == QUIT)
						return false;
				}
			}
			if (state == RUNNING)
				return true;
		}
	}
	// the instruction about to run is a step's first
	const bool go = serve();
	if (go && counting)
		steps_left--;
	return go;
}

void Prompt::faulted(const std::string& why)
{
	state = FAULTED;
	say(why + " at $" + hex(cpu.get_pc(), 4) + "\n" + regs());
	while (serve())
		;
}
