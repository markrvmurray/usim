//
//	test_monitor.cpp
//	The machine's command table, on its own: a command is its first
//	word, the rest its arguments; "help" lists every command in the
//	order added; a name added again replaces the first in its place;
//	and a line naming no command is left to the front end to answer.
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstdlib>
#include <string>

#include "mc6809.h"
#include "monitor.h"

static int failures = 0;

static void expect(const char* what, bool ok, const std::string& got = "")
{
	if (!ok) {
		fprintf(stderr, "FAIL %s\n%s\n", what, got.c_str());
		++failures;
	}
}

int main()
{
	Monitor m;
	std::string out, seen;

	m.add("echo", "echo ARGS         say them back\n",
		[&](const std::string& args, std::string& o) { seen = args; o += "[" + args + "]\n"; });
	m.add("two", "two               first\ntwo more          second line\n",
		[](const std::string&, std::string& o) { o += "two\n"; });

	expect("a command runs", m.run("echo hi", out) && out == "[hi]\n", out);
	out.clear();
	expect("arguments are trimmed, inner spaces kept",
		m.run("  echo   a  b \t\n", out) && seen == "a  b", seen);
	out.clear();
	expect("no arguments", m.run("echo", out) && seen.empty() && out == "[]\n", out);
	out.clear();
	expect("output is appended", m.run("two", out) && m.run("two", out) &&
		out == "two\ntwo\n", out);
	out.clear();

	expect("an unknown command is the front end's to answer",
		!m.run("frobnicate now", out) && out.empty(), out);
	expect("a prefix is not a command", !m.run("ech", out), out);

	const std::string help =
		"help              this list\n"
		"echo ARGS         say them back\n"
		"two               first\ntwo more          second line\n";
	expect("help, in the order added", m.run("help", out) && out == help, out);
	out.clear();
	expect("an empty line is help", m.run("   ", out) && out == help, out);
	out.clear();

	m.add("echo", "echo ARGS         shout them back\n",
		[](const std::string& args, std::string& o) { o += args + "!\n"; });
	expect("a name added again replaces the first",
		m.run("echo hi", out) && out == "hi!\n", out);
	out.clear();
	expect("in the same place in the help", m.help() ==
		"help              this list\n"
		"echo ARGS         shout them back\n"
		"two               first\ntwo more          second line\n", m.help());

	// the CPU's commands
	mc6809 cpu;
	Monitor c;
	add_cpu_commands(c, cpu);
	expect("cycles", c.run("cycles", out) && out == "0\n", out);
	out.clear();
	expect("trace on", c.run("trace on", out) && out.empty(), out);
	expect("trace off", c.run("trace off", out) && out.empty(), out);
	expect("trace, wrongly", c.run("trace", out) && out == "usage: trace on|off\n", out);
	out.clear();
	expect("their help", c.help() ==
		"help              this list\n"
		"trace on|off      the instruction trace, to stderr\n"
		"cycles            the CPU's total cycles\n", c.help());

	if (failures) {
		printf("test_monitor: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_monitor: PASS\n");
	return EXIT_SUCCESS;
}
