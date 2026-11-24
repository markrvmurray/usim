//
//	monitor.cpp
//	See monitor.h.
//
//	vim: ts=8 sw=8 noet:
//

#include "monitor.h"
#include "usim.h"

static std::string trim(const std::string& s)
{
	const char* space = " \t\r\n";
	size_t from = s.find_first_not_of(space);
	if (from == std::string::npos)
		return "";
	return s.substr(from, s.find_last_not_of(space) - from + 1);
}

Monitor::Monitor()
{
	add("help", "help              this list\n",
		[this](const std::string&, std::string& out) { out += help(); });
}

void Monitor::add(const std::string& name, const std::string& help, Handler run)
{
	for (auto& c : commands) {
		if (c.name == name) {
			c.help = help;
			c.run = std::move(run);
			return;
		}
	}
	commands.push_back({ name, help, std::move(run) });
}

bool Monitor::run(const std::string& line, std::string& out) const
{
	const std::string cmd = trim(line);
	const size_t end = cmd.find_first_of(" \t");
	const std::string name = cmd.empty() ? "help" : cmd.substr(0, end);
	const std::string args = end == std::string::npos ? "" : trim(cmd.substr(end));
	for (auto& c : commands) {
		if (c.name == name) {
			c.run(args, out);
			return true;
		}
	}
	return false;
}

std::string Monitor::help() const
{
	std::string out;
	for (auto& c : commands)
		out += c.help;
	return out;
}

void add_cpu_commands(Monitor& monitor, USim& cpu)
{
	monitor.add("trace", "trace on|off      the instruction trace, to stderr\n",
		[&cpu](const std::string& args, std::string& out) {
			if (args == "on")
				cpu.tron();
			else if (args == "off")
				cpu.troff();
			else
				out += "usage: trace on|off\n";
		});
	monitor.add("cycles", "cycles            the CPU's total cycles\n",
		[&cpu](const std::string&, std::string& out) {
			out += std::to_string(cpu.get_total_cycles()) + "\n";
		});
}
