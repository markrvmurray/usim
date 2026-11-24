//
//	prompt.h
//	A command prompt on the emulator's own terminal, for a machine
//	whose guest console is elsewhere (a pty, say): the guest is held
//	at reset until told to run, and can be stopped, stepped and looked
//	at, with the machine's own debug commands alongside.
//
//	Like GdbStub it is synchronous. The run loop calls before_insn()
//	ahead of every instruction; while the CPU is stopped that call
//	reads and runs commands, and returns only when it is told to go
//	on. While the CPU runs, typed lines are read between instructions,
//	so "stop" and the look-only commands work without stopping it.
//
//	Ctrl-C stops the CPU (catch_interrupts() arranges it). The end of
//	input stops nothing while the CPU runs, so a script can say "run"
//	and let the run end its own way; at the prompt it means "quit".
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "mc6809.h"
#include "monitor.h"

class Prompt {
public:
	// Reads commands from in_fd and writes to out. machine: the
	// machine's debug commands, offered alongside the prompt's own.
	Prompt(mc6809& cpu, const Monitor& machine, int in_fd = 0, FILE* out = stdout);

	// What memory the "mem" command reads: by default the CPU's
	// address space as it stands, without cycles or side effects.
	std::function<Byte(Word)>	read_memory;

	// Called about twenty times a second while the CPU is stopped, so
	// a machine's window, say, stays alive.
	std::function<void()>		idle;

	std::string			name = "usim";	// shown in the prompt

	// Before every instruction. False once told to quit.
	bool		before_insn();

	// The CPU met an invalid instruction: it stays stopped, open to
	// inspection, until told to quit.
	void		faulted(const std::string& why);

	// Ctrl-C stops the CPU (SIGINT, from then on, for the process).
	static void	catch_interrupts();

private:
	mc6809&		cpu;
	Monitor		own;		// run, step, regs ..., then the machine's
	int		in_fd;
	FILE*		out;

	enum State { STOPPED, RUNNING, FAULTED, QUIT };
	State		state = STOPPED;
	bool		started = false;	// the first before_insn() has been
	bool		counting = false;	// step N: steps_left is in force
	unsigned long	steps_left = 0;
	unsigned	poll_countdown = 0;
	bool		at_eof = false;
	std::vector<bool> breakpoints;		// by address
	std::string	pending;		// read, not yet a whole line

	void		stop(const std::string& why);
	bool		serve();
	void		run_line(const std::string& line);
	bool		read_input(bool wait);
	void		say(const std::string& text);
	std::string	regs() const;
};
