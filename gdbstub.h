//
//	gdbstub.h
//	A GDB remote serial protocol server, so lldb (or gdb) can debug the
//	emulated MC6809 live: registers, memory, single step, continue,
//	breakpoints and Ctrl-C.
//
//	It describes the CPU exactly as MAME's gdbstub does (architecture
//	"m6809"; registers A, B, D, X, Y, U, PC, S, CC, DP), so the same
//	lldb attaches to either emulator unchanged. The HD6309 is debugged
//	through the same 6809 register set, as in MAME.
//
//	The stub is synchronous. The run loop calls before_insn() ahead of
//	every instruction; while the CPU is stopped that call serves the
//	client's packets and returns only when it resumes. Guest time is
//	cycle-driven, so nothing in the machine moves while it is stopped,
//	and memory the debugger reads or writes costs no cycles.
//
//	Breakpoints compare the PC rather than patching code, so software
//	(Z0) and hardware (Z1) breakpoints behave the same.
//
//	"monitor" commands (qRcmd) reach the stub's own (help, trace,
//	cycles) and then the machine's, through the monitor hook. A
//	machine can also change what the debugger's memory accesses
//	reach, as usim09pt does to look through another DAT task.
//
//	The end of the program is a stop first, with the machine still
//	inspectable; continuing from there reports the exit status. An
//	invalid instruction stops with SIGILL the same way.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "mc6809.h"

class GdbStub {

	mc6809&			cpu;
	int			fd = -1;
	bool			ack = true;		// until QStartNoAckMode
	std::vector<bool>	breakpoints;		// by address
	bool			stepping = false;
	bool			resumed = false;	// first instruction after a resume
	bool			running = false;	// resumed, stop not yet reported
	unsigned		poll_countdown = 0;
	std::string		last_stop;		// for '?'
	std::string		rx;			// bytes received, not yet a packet

	bool			read_more();
	bool			next_packet(std::string& packet);
	void			send(const std::string& payload);

	std::string		stop_reply(int signal, bool breakpoint);
	std::string		registers();
	bool			set_registers(const std::string& hex);
	std::string		reg(unsigned n);
	bool			set_reg(unsigned n, unsigned value);
	std::string		query(const std::string& q);
	std::string		monitor_command(const std::string& cmd);

	// Serve packets until the client resumes (true) or ends (false).
	bool			serve(const std::string& stop);
	void			close_fd();

public:
	static const int	DEFAULT_PORT = 2159;	// as MAME's

	explicit		GdbStub(mc6809& cpu);

	// What the debugger's memory reads and writes reach: by default the
	// CPU's address space as it stands, without cycles or side effects.
	std::function<Byte(Word)>	read_memory;
	std::function<void(Word, Byte)>	write_memory;

	// The machine's monitor commands: true if it knows cmd, with its
	// output (lines ending in newlines) in out.
	std::function<bool(const std::string& cmd, std::string& out)> monitor;
	std::string			monitor_help;	// its lines for "help"
				~GdbStub();

	// Wait for one client. Port 0 takes any free port; port_file, if
	// given, receives the port number once it is listening, so a test
	// can start the debugger without guessing.
	bool			listen(int port, const char* port_file = nullptr);
	void			connect(int fd);	// or take an open socket
	bool			connected() const { return fd >= 0; }

	// Before every instruction. False once the client has killed the
	// session or gone away; detaching leaves the program running.
	bool			before_insn();

	// The program ended, by its own exit or by an invalid instruction.
	// Both stop for inspection first and return when the client lets go.
	void			exited(int code);
	void			faulted();
};
