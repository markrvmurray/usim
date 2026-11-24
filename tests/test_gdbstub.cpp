//
//	test_gdbstub.cpp
//	The GDB remote protocol stub, driven over a socket pair the way lldb
//	drives it: the stop at reset, the CPU description, registers,
//	memory, single step, breakpoints, watchpoints, monitor commands,
//	Ctrl-C, kill, detach, and the stops at the program's exit and at
//	an invalid instruction.
//
//	The emulator runs on its own thread, as the run loop in usim09batch
//	does; the test is the client.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <chrono>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include "gdbstub.h"
#include "haltdev.h"
#include "mc6809.h"
#include "memory.h"

static int failures = 0;

static void expect(const char* what, bool ok, const std::string& got = "")
{
	if (!ok) {
		fprintf(stderr, "FAIL %s: got \"%s\"\n", what, got.c_str());
		++failures;
	}
}

static bool has(const std::string& s, const char* part)
{
	return s.find(part) != std::string::npos;
}

// A 6809 with RAM, an exit device at $FFCC and a program at $0400
struct Machine {
	mc6809				cpu;
	bool				halted = false;
	bool				invalid = false;
	std::shared_ptr<HaltDevice>	halt;

	Machine(std::initializer_list<Byte> code)
	{
		auto ram = std::make_shared<RAM>(0x10000);
		halt = std::make_shared<HaltDevice>(cpu, &halted);
		cpu.attach_range(halt, 0xffcc, 0x0001);
		cpu.attach(ram, 0x0000, 0x0000);
		Word at = 0x0400;
		for (Byte c : code)
			ram->write(at++, c);
		ram->write(0xfffe, 0x04);
		ram->write(0xffff, 0x00);
		cpu.reset();
		cpu.abort = [this]() { invalid = true; };
	}

	// usim09batch's run loop: the exit code, -1 if killed, -2 if faulted
	int run(GdbStub& gdb)
	{
		while (!halted) {
			if (!gdb.before_insn())
				return -1;
			cpu.tick();
			if (invalid) {
				cpu.set_pc(cpu.get_insn_pc());
				gdb.faulted();
				return -2;
			}
		}
		gdb.exited(halt->getExitCode());
		return halt->getExitCode();
	}
};

// The debugger's end of the connection
struct Client {
	int		fd;
	bool		ack = true;
	std::string	rx;

	void send_raw(const std::string& s) { (void)!write(fd, s.data(), s.size()); }

	void send(const std::string& payload)
	{
		unsigned sum = 0;
		for (unsigned char c : payload)
			sum += c;
		char tail[4];
		snprintf(tail, sizeof tail, "#%02x", sum & 0xff);
		send_raw("$" + payload + tail);
	}

	// The next reply's payload, or "<closed>"
	std::string reply()
	{
		for (;;) {
			size_t start = rx.find('$');
			size_t hash = start == std::string::npos ? start : rx.find('#', start);
			if (hash != std::string::npos && hash + 2 < rx.size()) {
				std::string payload = rx.substr(start + 1, hash - start - 1);
				rx.erase(0, hash + 3);
				if (ack)
					send_raw("+");
				return payload;
			}
			char buf[512];
			ssize_t n = read(fd, buf, sizeof buf);
			if (n <= 0)
				return "<closed>";
			rx.append(buf, (size_t)n);
		}
	}

	std::string cmd(const std::string& payload)
	{
		send(payload);
		return reply();
	}

	// A monitor command's output, and whether it ended in OK
	std::string monitor(const std::string& command, bool& ok)
	{
		std::string hex = "qRcmd,";
		for (unsigned char ch : command) {
			char b[3];
			snprintf(b, sizeof b, "%02x", ch);
			hex += b;
		}
		send(hex);
		std::string out, r;
		while ((r = reply())[0] == 'O' && r != "OK")
			for (size_t i = 1; i + 1 < r.size(); i += 2)
				out += (char)strtoul(r.substr(i, 2).c_str(), nullptr, 16);
		ok = r == "OK";
		return out;
	}
};

// Runs code under the stub with a client attached
struct Session {
	Machine		m;
	std::unique_ptr<GdbStub> stub;
	GdbStub&	gdb;
	Client		c;
	int		result = 0;
	std::thread	emu;

	// A stub with just the CPU's monitor commands, or with a machine's
	Session(std::initializer_list<Byte> code, Monitor* commands = nullptr)
		: m(code),
		  stub(commands ? std::make_unique<GdbStub>(m.cpu, *commands)
				: std::make_unique<GdbStub>(m.cpu)),
		  gdb(*stub)
	{
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			perror("socketpair");
			exit(EXIT_FAILURE);
		}
		gdb.connect(sv[0]);
		c.fd = sv[1];
		emu = std::thread([this]() { result = m.run(gdb); });
	}

	int finish()
	{
		emu.join();
		close(c.fd);
		return result;
	}
};

// The value of register n ("06") in a stop reply
static std::string stop_reg(const std::string& stop, const char* n)
{
	size_t i = stop.find(std::string(n) + ":");
	return i == std::string::npos ? "" : stop.substr(i + 3, stop.find(';', i) - i - 3);
}

static void test_session()
{
	Session s({
		0x86, 0x12,		// $0400 LDA #$12
		0xC6, 0x34,		// $0402 LDB #$34
		0xB7, 0x10, 0x00,	// $0404 STA $1000
		0x8E, 0x56, 0x78,	// $0407 LDX #$5678
		0x86, 0x2A,		// $040A LDA #$2A
		0xB7, 0xFF, 0xCC,	// $040C STA $FFCC (exit $2A)
		0x20, 0xFE,		// $040F BRA *
	});
	Client& c = s.c;
	std::string r;

	r = c.cmd("qSupported:multiprocess+;swbreak+;hwbreak+");
	expect("qSupported", has(r, "qXfer:features:read+") && has(r, "QStartNoAckMode+"), r);
	r = c.cmd("QStartNoAckMode");
	expect("QStartNoAckMode", r == "OK", r);
	c.ack = false;

	r = c.cmd("qXfer:features:read:target.xml:0,fff");
	expect("target.xml", r[0] == 'l' && has(r, "<architecture>m6809</architecture>") &&
		has(r, "name=\"pc\" bitsize=\"16\""), r);
	r = c.cmd("qXfer:features:read:target.xml:0,10");
	expect("target.xml in parts", r[0] == 'm' && r.size() == 17, r);

	// "mc6809-unknown-none", hex-encoded
	r = c.cmd("qHostInfo");
	expect("qHostInfo", has(r, "triple:6d63363830392d756e6b6e6f776e2d6e6f6e65;") &&
		has(r, "endian:big;"), r);

	r = c.cmd("?");
	expect("stop at reset", r.rfind("T05", 0) == 0 && stop_reg(r, "06") == "0400", r);
	r = c.cmd("g");
	expect("g carries the PC", r.size() == 32 && r.substr(20, 4) == "0400", r);

	r = c.cmd("s");
	expect("step", r.rfind("T05", 0) == 0 && !has(r, "swbreak") && stop_reg(r, "06") == "0402", r);
	r = c.cmd("p0");
	expect("A after LDA", r == "12", r);

	r = c.cmd("Z0,40a,1");
	expect("set breakpoint", r == "OK", r);
	r = c.cmd("c");
	expect("breakpoint", has(r, "swbreak:;") && stop_reg(r, "06") == "040a", r);
	expect("D", c.cmd("p2") == "1234");
	expect("X", c.cmd("p3") == "5678");
	r = c.cmd("m1000,1");
	expect("memory the program wrote", r == "12", r);

	expect("set A", c.cmd("P0=99") == "OK");
	r = c.cmd("p2");
	expect("D follows A", r == "9934", r);
	expect("write memory", c.cmd("M2000,2:abcd") == "OK");
	r = c.cmd("m2000,2");
	expect("read it back", r == "abcd", r);

	// continuing from the breakpoint runs through it to the exit,
	// which stops first with the machine intact
	r = c.cmd("c");
	expect("stop at exit", r.rfind("T05", 0) == 0 && stop_reg(r, "06") == "040f", r);
	r = c.cmd("p0");
	expect("inspect at exit", r == "2a", r);
	r = c.cmd("c");
	expect("exit status", r == "W2a", r);
	expect("exit code", s.finish() == 0x2a);
}

static void test_interrupt_and_kill()
{
	Session s({ 0x20, 0xFE });		// BRA *
	Client& c = s.c;

	c.cmd("?");
	c.send("c");
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	c.send_raw("\x03");
	std::string r = c.reply();
	expect("Ctrl-C", r.rfind("T02", 0) == 0 && stop_reg(r, "06") == "0400", r);
	std::string k = c.cmd("k");
	expect("kill reply", k == "X09", k);
	expect("kill", s.finish() == -1);
}

static void test_detach()
{
	Session s({ 0x86, 0x07, 0xB7, 0xFF, 0xCC });	// exit 7
	Client& c = s.c;

	c.cmd("?");
	c.cmd("Z0,402,1");
	std::string r = c.cmd("D");
	expect("detach", r == "OK", r);
	expect("detached run ignores breakpoints", s.finish() == 7);
}

static void test_invalid()
{
	Session s({ 0x12, 0x14 });		// NOP, then an opcode with no model
	Client& c = s.c;

	c.cmd("?");
	std::string r = c.cmd("c");
	expect("SIGILL", r.rfind("T04", 0) == 0 && stop_reg(r, "06") == "0401", r);
	r = c.cmd("c");
	expect("terminated", r == "X04", r);
	expect("faulted", s.finish() == -2);
}

static void test_watchpoints()
{
	Session s({
		0xB6, 0x20, 0x00,	// $0400 LDA $2000
		0xB7, 0x30, 0x00,	// $0403 STA $3000
		0xF6, 0x30, 0x00,	// $0406 LDB $3000
		0x12,			// $0409 NOP
		0xB7, 0x20, 0x00,	// $040A STA $2000
		0x86, 0x05,		// $040D LDA #5
		0xB7, 0xFF, 0xCC,	// $040F STA $FFCC (exit 5)
	});
	Client& c = s.c;
	std::string r;

	c.cmd("?");
	expect("watchpoints stop after the access",
		has(c.cmd("qHostInfo"), "watchpoint_exceptions_received:after;"));
	expect("watchpoint count", c.cmd("qWatchpointSupportInfo:").rfind("num:", 0) == 0);

	// a read watchpoint on code: fetching it is not a data read
	expect("Z3 on code", c.cmd("Z3,401,2") == "OK");
	expect("Z2 $3000", c.cmd("Z2,3000,1") == "OK");
	r = c.cmd("c");
	expect("write watchpoint", has(r, "watch:3000;") && !has(r, "rwatch") &&
		stop_reg(r, "06") == "0406", r);
	expect("z2", c.cmd("z2,3000,1") == "OK");
	expect("z3", c.cmd("z3,401,2") == "OK");

	// a two-byte read watchpoint is hit by a read of either byte
	expect("Z3 $2FFF+2", c.cmd("Z3,2fff,2") == "OK");
	r = c.cmd("c");
	expect("read watchpoint", has(r, "rwatch:3000;") && stop_reg(r, "06") == "0409", r);
	expect("z3 $2FFF+2", c.cmd("z3,2fff,2") == "OK");

	// an access watchpoint is hit by the write, and reports awatch
	expect("Z4 $2000", c.cmd("Z4,2000,1") == "OK");
	r = c.cmd("c");
	expect("access watchpoint", has(r, "awatch:2000;") && stop_reg(r, "06") == "040d", r);

	// detaching takes the watchpoints with it
	expect("detach", c.cmd("D") == "OK");
	expect("exit code", s.finish() == 5);
	expect("detach clears the flags", s.m.cpu.watch_flags == nullptr);
}

static void test_monitor()
{
	bool ok;
	std::string out;
	{
		// a stub of its own: the CPU's commands and no others
		Session s({ 0x12, 0x20, 0xFE });	// NOP; BRA *
		Client& c = s.c;
		c.cmd("?");
		out = c.monitor("help", ok);
		expect("the CPU's commands", ok && has(out, "trace on|off") &&
			has(out, "cycles") && !has(out, "the machine's command"), out);
		out = c.monitor("cycles", ok);
		expect("cycles", ok && out == "0\n", out);
		c.cmd("s");
		// the total lags the instruction just run (USim folds it in
		// at the next tick), so only that it moves is certain
		out = c.monitor("cycles", ok);
		expect("cycles after a NOP", ok && strtoul(out.c_str(), nullptr, 10) > 0, out);
		out = c.monitor("trace sideways", ok);
		expect("a command's own usage", ok && out == "usage: trace on|off\n", out);
		out = c.monitor("hello", ok);
		expect("no machine commands", ok && has(out, "unknown command: hello"), out);
		c.send("k");
		expect("kill", s.finish() == -1);
	}

	// a machine's commands, shared with the stub, and its view of memory
	Monitor commands;
	commands.add("hello", "hello             the machine's command\n",
		[](const std::string& args, std::string& out) {
			out += "hello from the machine" + (args.empty() ? "" : ", " + args) + "\n";
		});
	Session s({ 0x12, 0x20, 0xFE }, &commands);
	Client& c = s.c;
	add_cpu_commands(commands, s.m.cpu);
	s.gdb.read_memory = [](Word a) { return (Byte)(a & 0xff); };

	c.cmd("?");
	out = c.monitor("help", ok);
	expect("help", ok && has(out, "trace on|off") && has(out, "the machine's command"), out);
	out = c.monitor("hello", ok);
	expect("machine command", ok && out == "hello from the machine\n", out);
	out = c.monitor("  hello   there  ", ok);
	expect("its arguments", ok && out == "hello from the machine, there\n", out);
	out = c.monitor("frobnicate", ok);
	expect("unknown command", ok && has(out, "unknown command: frobnicate"), out);
	std::string r = c.cmd("m1234,2");
	expect("memory view hook", r == "3435", r);

	c.send("k");
	expect("kill", s.finish() == -1);
}

// A debugger's memory access costs the guest no cycles
static void test_cycle_neutral()
{
	Machine plain({ 0x12 }), probed({ 0x12 });	// NOP
	for (int i = 0; i < 100; i++) {
		probed.cpu.debug_read((Word)(0x2000 + i));
		probed.cpu.debug_write((Word)(0x2000 + i), 0x55);
	}
	plain.cpu.tick();
	plain.cpu.tick();
	probed.cpu.tick();
	probed.cpu.tick();
	expect("debug access costs no cycles",
		plain.cpu.get_total_cycles() == probed.cpu.get_total_cycles());
}

int main()
{
	test_session();
	test_interrupt_and_kill();
	test_detach();
	test_invalid();
	test_watchpoints();
	test_monitor();
	test_cycle_neutral();

	if (failures) {
		printf("test_gdbstub: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_gdbstub: PASS\n");
	return EXIT_SUCCESS;
}
