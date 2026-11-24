//
//	monitor.h
//	A machine's debug commands, in one table, so that every front end
//	that offers them -- a debugger's "monitor" command, or a prompt on
//	the emulator's own terminal -- runs the same code and prints the
//	same help.
//
//	A command is its first word; what follows is handed to it as its
//	arguments. "help" lists every command, in the order they were
//	added. Front ends word their own reply to a command that is not
//	there, since each has its own way of being asked for help.
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include <functional>
#include <string>
#include <vector>

class USim;

class Monitor {
public:
	// Runs a command with its arguments (already trimmed), appending
	// its output, as lines ending in newlines, to out.
	using Handler = std::function<void(const std::string& args, std::string& out)>;

	Monitor();

	// A command, and its help: one or more lines, each ending in a
	// newline, in the form "usage   what it does". A name added
	// twice replaces the first, keeping its place in the help.
	void		add(const std::string& name, const std::string& help, Handler run);

	// Runs one line. False if its first word names no command; an
	// empty line is "help".
	bool		run(const std::string& line, std::string& out) const;

	std::string	help() const;

private:
	struct Command {
		std::string	name;
		std::string	help;
		Handler		run;
	};
	std::vector<Command>	commands;
};

// The commands every CPU has: the instruction trace, and its cycles.
void add_cpu_commands(Monitor& monitor, USim& cpu);
