//
//	test_traps.cpp
//	usim09pt's crash detectors, run on small programs: --trap-sled
//	stops on a jump onto a $00 sled whatever else is set, and only
//	then; the UniFLEX-only checks stay out of other guests' way; and a
//	detector's stop is reported as a suspicion, distinct from a crash,
//	in both its headline and its exit status.
//
//	vim: ts=8 sw=8 noet:
//

// mkdtemp and usleep: beyond the POSIX.1 of 1988 the build asks for
// (macOS also wants its own switch for them)
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <csignal>
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

static bool has(const std::string& s, const char* part)
{
	return s.find(part) != std::string::npos;
}

static char dir[] = "/tmp/test_traps_XXXXXX";

// An S-record file holding code at $E000 (the fixed window, the same
// in every DAT task) and a reset vector pointing at it
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
	std::string path = std::string(dir) + "/" + name + ".s19";
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

// usim09pt's exit status (128 + N if signal N ended it, -1 if it ran
// past the wall-clock limit), with its stderr in out. The limit turns a
// detector that silently never fires into a failure, not a hang.
static int run(const std::vector<std::string>& args, std::string& out)
{
	const std::string err = std::string(dir) + "/stderr";
	pid_t pid = fork();
	if (pid == 0) {
		freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen(err.c_str(), "w", stderr);
		std::vector<std::string> argv = { "./usim09pt",
			"-f", std::string(dir) + "/fram", "--rtc-time=2026-01-01T00:00:00" };
		argv.insert(argv.end(), args.begin(), args.end());
		std::vector<char*> cargv;
		for (auto& a : argv)
			cargv.push_back(a.data());
		cargv.push_back(nullptr);
		execv(cargv[0], cargv.data());
		_exit(127);
	}

	int status = 0, waited = 0;
	while (waitpid(pid, &status, WNOHANG) == 0) {
		if (++waited > 20 * 20) {		// 20 seconds
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			status = -1;
			break;
		}
		usleep(50000);
	}

	out.clear();
	if (FILE* f = fopen(err.c_str(), "r")) {
		char buf[512];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0)
			out.append(buf, n);
		fclose(f);
	}
	if (status == -1) {
		out += "(still running after 20s; killed)";
		return -1;
	}
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}

int main()
{
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return EXIT_FAILURE;
	}
	std::string out;
	int rc;

	// NitrOS-9's own task switch, in miniature: task 2 mapped, then an
	// ordinary branch within the fixed page
	const std::string task_switch = program("switch", {
		0x10, 0xCE, 0xE8, 0x00,		// lds  #$E800
		0x86, 0x02,			// lda  #2
		0xB7, 0xFF, 0xC0,		// sta  $FFC0 (task 2)
		0x26, 0x08,			// bne  +8
		0x12, 0x12, 0x12, 0x12,		// nop x 8
		0x12, 0x12, 0x12, 0x12,
		0x20, 0xFE,			// bra  *
	});
	rc = run({ "--trap-sled", "--timeout=50", task_switch }, out);
	expect("--trap-sled ignores a branch in the fixed page", rc == 124 &&
		!has(out, "POST-MORTEM") && !has(out, "HEURISTIC"), out);
	rc = run({ "--trap-uniflex", "--timeout=50", task_switch }, out);
	expect("--trap-uniflex flags it, as a suspicion", rc == 3 &&
		has(out, "==== HEURISTIC STOP (not a confirmed crash): UniFLEX") &&
		!has(out, "CRASH POST-MORTEM"), out);

	// a jump onto zeroed RAM, with nothing else to engage the step loop
	const std::string sled = program("sled", {
		0x10, 0xCE, 0xE8, 0x00,		// lds  #$E800
		0x7E, 0x10, 0x00,		// jmp  $1000
	});
	rc = run({ "--trap-sled", sled }, out);
	expect("--trap-sled stops on a sled, without --timeout", rc == 3 &&
		has(out, "HEURISTIC STOP (not a confirmed crash): jump onto a $00 sled $E004 -> $1000"), out);

	// a real crash is still reported as one
	const std::string invalid = program("invalid", {
		0x10, 0xCE, 0xE8, 0x00,		// lds  #$E800
		0x14,				// no such instruction
	});
	rc = run({ "--timeout=50", invalid }, out);
	expect("an invalid instruction is a crash",
		has(out, "==== CRASH POST-MORTEM: invalid instruction ====") &&
		!has(out, "HEURISTIC"), out);

	std::string cleanup = std::string("rm -rf ") + dir;
	(void)!system(cleanup.c_str());

	if (failures) {
		printf("test_traps: FAIL (%d)\n", failures);
		return EXIT_FAILURE;
	}
	printf("test_traps: PASS\n");
	return EXIT_SUCCESS;
}
