//
//	gdbstub.cpp
//	GDB remote serial protocol server (see gdbstub.h)
//
//	vim: ts=8 sw=8 noet:
//

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gdbstub.h"

// Instructions between checks for a Ctrl-C while the program runs
static const unsigned	POLL_INTERVAL = 4096;

static const int	SIGINT_ = 2, SIGILL_ = 4, SIGTRAP_ = 5, SIGKILL_ = 9;

// Watchpoints cost the same however many there are; the limit only
// keeps a runaway client from building an unbounded list.
static const unsigned	MAX_WATCHPOINTS = 64;

// The register set, in MAME's order and numbering
static const struct {
	const char*	name;
	unsigned	bits;
	const char*	type;
} REGS[] = {
	{ "a",  8,  "int" },
	{ "b",  8,  "int" },
	{ "d",  16, "int" },
	{ "x",  16, "int" },
	{ "y",  16, "int" },
	{ "u",  16, "data_ptr" },
	{ "pc", 16, "code_ptr" },
	{ "s",  16, "data_ptr" },
	{ "cc", 8,  "int" },
	{ "dp", 8,  "int" },
};
static const unsigned	NREGS = sizeof(REGS) / sizeof(REGS[0]);
static const unsigned	REG_U = 5, REG_PC = 6, REG_S = 7;

// lldb's name for the target, hex-encoded as qHostInfo carries it
static const std::string TRIPLE_HEX = [] {
	std::string hex;
	for (char c : std::string("mc6809-unknown-none"))
		hex += "0123456789abcdef"[(c >> 4) & 0xf], hex += "0123456789abcdef"[c & 0xf];
	return hex;
}();

static std::string target_xml()
{
	// The feature name is MAME's, so a client keyed to it sees no
	// difference between the two emulators.
	std::string xml =
		"<?xml version=\"1.0\"?>\n"
		"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
		"<target version=\"1.0\">\n"
		"<architecture>m6809</architecture>\n"
		"  <feature name=\"mame.m6809\">\n";
	for (const auto& r : REGS) {
		char line[96];
		snprintf(line, sizeof line, "    <reg name=\"%s\" bitsize=\"%u\" type=\"%s\"/>\n",
			r.name, r.bits, r.type);
		xml += line;
	}
	xml += "  </feature>\n</target>\n";
	return xml;
}

static std::string hex8(unsigned v)
{
	char buf[3];
	snprintf(buf, sizeof buf, "%02x", v & 0xff);
	return buf;
}

// Parses hex digits from s at pos, stopping at the first non-digit
static bool parse_hex(const std::string& s, size_t& pos, unsigned long& value)
{
	size_t start = pos;
	value = 0;
	while (pos < s.size() && isxdigit((unsigned char)s[pos])) {
		char c = s[pos++];
		value = value * 16 + (isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10));
	}
	return pos > start;
}

GdbStub::GdbStub(mc6809& cpu)
	: cpu(cpu), breakpoints(0x10000, false),
	  watch_flags(0x10000, 0), watch_access(0x10000, false)
{
	read_memory = [&cpu](Word a) { return cpu.debug_read(a); };
	write_memory = [&cpu](Word a, Byte v) { cpu.debug_write(a, v); };
}

GdbStub::~GdbStub()
{
	close_fd();
}

bool GdbStub::listen(int port, const char* port_file)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0) {
		perror("gdbstub: socket");
		return false;
	}
	int one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

	// localhost only: the stub can write any byte of the machine
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (bind(ls, (sockaddr*)&addr, sizeof addr) < 0 || ::listen(ls, 1) < 0) {
		perror("gdbstub: bind");
		close(ls);
		return false;
	}

	socklen_t len = sizeof addr;
	getsockname(ls, (sockaddr*)&addr, &len);
	port = ntohs(addr.sin_port);
	if (port_file) {
		// written whole, then renamed, so a reader never sees half
		std::string tmp = std::string(port_file) + ".tmp";
		FILE* f = fopen(tmp.c_str(), "w");
		if (!f || fprintf(f, "%d\n", port) < 0 || fclose(f) != 0 ||
		    rename(tmp.c_str(), port_file) != 0) {
			perror(port_file);
			close(ls);
			return false;
		}
	}
	fprintf(stderr, "gdbstub: waiting for a debugger on localhost:%d\n", port);
	int s = accept(ls, nullptr, nullptr);
	close(ls);
	if (s < 0) {
		perror("gdbstub: accept");
		return false;
	}
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	connect(s);
	return true;
}

void GdbStub::connect(int s)
{
	// a client that goes away must not take the emulator with it
	signal(SIGPIPE, SIG_IGN);
	fd = s;
	ack = true;
	rx.clear();

	// Stop before the first instruction, quietly: the client asks
	// why with '?' once it has set itself up.
	stepping = true;
	running = false;
}

void GdbStub::close_fd()
{
	if (fd >= 0)
		close(fd);
	fd = -1;
}

//----------------------------------------------------------------------------
// Packets
//----------------------------------------------------------------------------

bool GdbStub::read_more()
{
	char buf[1024];
	ssize_t n = recv(fd, buf, sizeof buf, 0);
	if (n <= 0)
		return false;
	rx.append(buf, (size_t)n);
	return true;
}

// The next packet's payload, or "\x03" for an interrupt. False once
// the connection is gone.
bool GdbStub::next_packet(std::string& packet)
{
	for (;;) {
		// acknowledgements and noise between packets
		size_t i = 0;
		while (i < rx.size() && rx[i] != '$' && rx[i] != '\x03')
			i++;
		rx.erase(0, i);

		if (!rx.empty() && rx[0] == '\x03') {
			rx.erase(0, 1);
			packet = "\x03";
			return true;
		}

		size_t hash = rx.find('#');
		if (!rx.empty() && hash != std::string::npos && hash + 2 < rx.size()) {
			std::string payload = rx.substr(1, hash - 1);
			unsigned want = (unsigned)strtoul(rx.substr(hash + 1, 2).c_str(), nullptr, 16);
			rx.erase(0, hash + 3);

			unsigned sum = 0;
			for (unsigned char c : payload)
				sum += c;
			if ((sum & 0xff) != want) {
				if (ack)
					::send(fd, "-", 1, 0);
				continue;
			}
			if (ack)
				::send(fd, "+", 1, 0);
			packet = payload;
			return true;
		}

		if (!read_more())
			return false;
	}
}

void GdbStub::send(const std::string& payload)
{
	std::string out = "$";
	unsigned sum = 0;
	for (unsigned char c : payload) {
		if (c == '$' || c == '#' || c == '}' || c == '*') {
			out += '}';
			sum += '}';
			c ^= 0x20;
		}
		out += (char)c;
		sum += c;
	}
	out += '#';
	out += hex8(sum);
	::send(fd, out.data(), out.size(), 0);
}

//----------------------------------------------------------------------------
// Registers
//----------------------------------------------------------------------------

std::string GdbStub::reg(unsigned n)
{
	unsigned v;
	switch (n) {
		case 0: v = cpu.get_a(); break;
		case 1: v = cpu.get_b(); break;
		case 2: v = (cpu.get_a() << 8) | cpu.get_b(); break;
		case 3: v = cpu.get_x(); break;
		case 4: v = cpu.get_y(); break;
		case 5: v = cpu.get_u(); break;
		case 6: v = cpu.get_pc(); break;
		case 7: v = cpu.get_s(); break;
		case 8: v = cpu.get_cc(); break;
		case 9: v = cpu.get_dp(); break;
		default: return "";
	}
	// big-endian, as the target is
	return REGS[n].bits == 16 ? hex8(v >> 8) + hex8(v) : hex8(v);
}

bool GdbStub::set_reg(unsigned n, unsigned v)
{
	switch (n) {
		case 0: cpu.set_a((Byte)v); break;
		case 1: cpu.set_b((Byte)v); break;
		case 2: cpu.set_a((Byte)(v >> 8)); cpu.set_b((Byte)v); break;
		case 3: cpu.set_x((Word)v); break;
		case 4: cpu.set_y((Word)v); break;
		case 5: cpu.set_u((Word)v); break;
		case 6: cpu.set_pc((Word)v); break;
		case 7: cpu.set_s((Word)v); break;
		case 8: cpu.set_cc((Byte)v); break;
		case 9: cpu.set_dp((Byte)v); break;
		default: return false;
	}
	return true;
}

std::string GdbStub::registers()
{
	std::string out;
	for (unsigned n = 0; n < NREGS; n++)
		out += reg(n);
	return out;
}

// D comes after A and B, so a G that changes only A or B and sends D
// unchanged undoes it: D is skipped unless it disagrees with A:B as sent.
bool GdbStub::set_registers(const std::string& hex)
{
	std::vector<unsigned> v;
	size_t pos = 0;
	for (unsigned n = 0; n < NREGS; n++) {
		size_t len = REGS[n].bits / 4;
		if (pos + len > hex.size())
			return false;
		v.push_back((unsigned)strtoul(hex.substr(pos, len).c_str(), nullptr, 16));
		pos += len;
	}
	for (unsigned n = 0; n < NREGS; n++)
		if (n != 2 || v[2] != ((v[0] << 8) | v[1]))
			set_reg(n, v[n]);
	return true;
}

std::string GdbStub::stop_reply(int signal, bool breakpoint, const std::string& why)
{
	std::string out = "T" + hex8(signal) + why;
	if (breakpoint)
		out += "swbreak:;";
	for (unsigned n : { REG_U, REG_PC, REG_S })
		out += hex8(n) + ":" + reg(n) + ";";
	return out;
}

//----------------------------------------------------------------------------
// Watchpoints
//----------------------------------------------------------------------------

// The flags USim tests are rebuilt from the list, so overlapping
// watchpoints come and go independently.
void GdbStub::set_watchpoint(bool insert, char type, Word addr, Word len)
{
	if (len == 0)
		len = 1;
	for (auto it = watchpoints.begin(); it != watchpoints.end(); ++it) {
		if (it->addr == addr && it->len == len && it->type == type) {
			if (!insert)
				watchpoints.erase(it);
			rebuild_watch_flags();
			return;
		}
	}
	if (insert)
		watchpoints.push_back({ addr, len, type });
	rebuild_watch_flags();
}

void GdbStub::rebuild_watch_flags()
{
	std::fill(watch_flags.begin(), watch_flags.end(), 0);
	std::fill(watch_access.begin(), watch_access.end(), false);
	for (const auto& w : watchpoints) {
		const Byte mask = w.type == '2' ? USim::WATCH_WRITE
				: w.type == '3' ? USim::WATCH_READ
				: USim::WATCH_READ | USim::WATCH_WRITE;
		for (unsigned i = 0; i < w.len; i++) {
			const Word a = (Word)(w.addr + i);
			watch_flags[a] |= mask;
			if (w.type == '4')
				watch_access[a] = true;
		}
	}
	// no watchpoints, no per-access test beyond the null pointer
	cpu.watch_flags = watchpoints.empty() ? nullptr : watch_flags.data();
	cpu.watch_hit = {};
}

void GdbStub::clear_watchpoints()
{
	watchpoints.clear();
	rebuild_watch_flags();
}

// The stop reply's account of a hit, in gdb's terms
std::string GdbStub::watch_reason(const USim::WatchHit& hit) const
{
	const char* kind = watch_access[hit.addr] ? "awatch"
			 : hit.kind == USim::WATCH_WRITE ? "watch" : "rwatch";
	return std::string(kind) + ":" + hex8(hit.addr >> 8) + hex8(hit.addr) + ";";
}

//----------------------------------------------------------------------------
// Monitor commands
//----------------------------------------------------------------------------

std::string GdbStub::monitor_command(const std::string& cmd)
{
	std::string out;
	if (cmd == "help" || cmd.empty()) {
		out =	"help              this list\n"
			"trace on|off      the instruction trace, to stderr\n"
			"cycles            the CPU's total cycles\n";
		out += monitor_help;
	} else if (cmd == "trace on") {
		cpu.tron();
	} else if (cmd == "trace off") {
		cpu.troff();
	} else if (cmd == "cycles") {
		out = std::to_string(cpu.get_total_cycles()) + "\n";
	} else if (!(monitor && monitor(cmd, out))) {
		out = "unknown command: " + cmd + " (try \"monitor help\")\n";
	}
	return out;
}

//----------------------------------------------------------------------------
// Queries
//----------------------------------------------------------------------------

std::string GdbStub::query(const std::string& q)
{
	if (q.rfind("qSupported", 0) == 0)
		return "PacketSize=1000;qXfer:features:read+;swbreak+;hwbreak+;QStartNoAckMode+";
	if (q == "QStartNoAckMode") {
		ack = false;		// the client acknowledges this OK itself
		return "OK";
	}
	if (q.rfind("qXfer:features:read:target.xml:", 0) == 0) {
		size_t pos = strlen("qXfer:features:read:target.xml:");
		unsigned long offset, length;
		if (!parse_hex(q, pos, offset) || pos >= q.size() || q[pos++] != ',' ||
		    !parse_hex(q, pos, length))
			return "E01";
		const std::string xml = target_xml();
		if (offset >= xml.size())
			return "l";
		std::string part = xml.substr(offset, length);
		return (offset + part.size() < xml.size() ? "m" : "l") + part;
	}
	// lldb's own queries. Without an ELF loaded, these are where it
	// learns the architecture; MAME leaves them out because its
	// sessions always start from one.
	// Watchpoints stop after the access, so lldb need not step over one.
	if (q == "qHostInfo")
		return "triple:" + TRIPLE_HEX + ";endian:big;ptrsize:2;"
		       "watchpoint_exceptions_received:after;";
	if (q == "qWatchpointSupportInfo" || q == "qWatchpointSupportInfo:")
		return "num:" + std::to_string(MAX_WATCHPOINTS) + ";";
	if (q.rfind("qRcmd,", 0) == 0) {
		std::string cmd;
		for (size_t i = 6; i + 1 < q.size(); i += 2)
			cmd += (char)strtoul(q.substr(i, 2).c_str(), nullptr, 16);
		std::string out = monitor_command(cmd);
		if (!out.empty()) {
			std::string hex = "O";
			for (unsigned char c : out)
				hex += hex8(c);
			send(hex);
		}
		return "OK";
	}
	if (q == "qProcessInfo")
		return "pid:1;triple:" + TRIPLE_HEX + ";endian:big;ptrsize:2;";
	if (q == "qC")
		return "QC1";
	if (q == "qfThreadInfo")
		return "m1";
	if (q == "qsThreadInfo")
		return "l";
	if (q == "qAttached")
		return "1";
	return "";
}

//----------------------------------------------------------------------------
// Running and stopping
//----------------------------------------------------------------------------

bool GdbStub::serve(const std::string& stop)
{
	last_stop = stop;
	if (running) {
		send(stop);
		running = false;
	}

	std::string p;
	while (next_packet(p)) {
		size_t pos = 1;
		unsigned long addr, len, value;

		switch (p[0]) {
		case '\x03':			// already stopped
			break;
		case '?':
			send(last_stop);
			break;
		case 'g':
			send(registers());
			break;
		case 'G':
			send(set_registers(p.substr(1)) ? "OK" : "E01");
			break;
		case 'p':
			if (parse_hex(p, pos, addr) && addr < NREGS)
				send(reg((unsigned)addr));
			else
				send("E01");
			break;
		case 'P':
			if (parse_hex(p, pos, addr) && pos < p.size() && p[pos++] == '=' &&
			    parse_hex(p, pos, value) && set_reg((unsigned)addr, (unsigned)value))
				send("OK");
			else
				send("E01");
			break;
		case 'm':
			if (parse_hex(p, pos, addr) && pos < p.size() && p[pos++] == ',' &&
			    parse_hex(p, pos, len)) {
				std::string out;
				for (unsigned long i = 0; i < len; i++)
					out += hex8(read_memory((Word)(addr + i)));
				send(out);
			} else
				send("E01");
			break;
		case 'M':
			if (parse_hex(p, pos, addr) && pos < p.size() && p[pos++] == ',' &&
			    parse_hex(p, pos, len) && pos < p.size() && p[pos++] == ':' &&
			    p.size() - pos >= 2 * len) {
				for (unsigned long i = 0; i < len; i++)
					write_memory((Word)(addr + i), (Byte)strtoul(p.substr(pos + 2 * i, 2).c_str(), nullptr, 16));
				send("OK");
			} else
				send("E01");
			break;
		case 'c':
		case 's':
			if (parse_hex(p, pos, addr))
				cpu.set_pc((Word)addr);
			stepping = p[0] == 's';
			resumed = true;
			running = true;
			return true;
		case 'Z':
		case 'z':
			// Ztype,addr,kind: kind is a breakpoint's size, ignored,
			// or the number of bytes a watchpoint covers
			if (p.size() > 2 && p[1] >= '0' && p[1] <= '4' && p[2] == ',' &&
			    (pos = 3, parse_hex(p, pos, addr)) && addr <= 0xffff &&
			    pos < p.size() && p[pos++] == ',' && parse_hex(p, pos, len)) {
				const bool insert = p[0] == 'Z';
				if (p[1] <= '1') {
					breakpoints[addr] = insert;
				} else if (insert && watchpoints.size() >= MAX_WATCHPOINTS) {
					send("E02");
					break;
				} else {
					set_watchpoint(insert, p[1], (Word)addr,
						(Word)std::min(len, 0xfffful));
				}
				send("OK");
			} else
				send("");
			break;
		case 'k':
			// gdb expects nothing back; lldb waits for the exit
			send("X" + hex8(SIGKILL_));
			close_fd();
			clear_watchpoints();
			return false;
		case 'D':
			send("OK");
			close_fd();
			breakpoints.assign(0x10000, false);
			clear_watchpoints();
			return true;
		case 'H':
		case 'T':
			send("OK");
			break;
		case 'q':
		case 'Q':
			send(query(p));
			break;
		default:
			send("");
			break;
		}
	}

	// the client has gone
	close_fd();
	clear_watchpoints();
	return false;
}

bool GdbStub::before_insn()
{
	if (fd < 0)
		return true;		// detached, or never attached: run on

	// A resume from a breakpoint executes the instruction there. A
	// watchpoint hit by the last instruction stops here, after it.
	const bool at_breakpoint = breakpoints[cpu.get_pc()] && !resumed;
	resumed = false;
	std::string why;
	if (cpu.watch_hit.hit) {
		why = watch_reason(cpu.watch_hit);
		cpu.watch_hit = {};
	}
	if (stepping || at_breakpoint || !why.empty()) {
		stepping = false;
		return serve(stop_reply(SIGTRAP_, at_breakpoint, why));
	}

	if (poll_countdown-- == 0) {
		poll_countdown = POLL_INTERVAL;
		pollfd pfd = { fd, POLLIN, 0 };
		if (poll(&pfd, 1, 0) > 0) {
			if (!read_more()) {
				close_fd();
				return false;
			}
			size_t i = rx.find('\x03');
			if (i != std::string::npos) {
				rx.erase(0, i + 1);
				return serve(stop_reply(SIGINT_, false));
			}
		}
	}
	return true;
}

void GdbStub::exited(int code)
{
	if (fd < 0)
		return;
	if (serve(stop_reply(SIGTRAP_, false)) && fd >= 0) {
		send("W" + hex8((unsigned)code));
		close_fd();
	}
}

void GdbStub::faulted()
{
	if (fd < 0)
		return;
	if (serve(stop_reply(SIGILL_, false)) && fd >= 0) {
		send("X" + hex8(SIGILL_));
		close_fd();
	}
}
