#
# usim (C) R.P.Bellis 1993 -
# vim: set ts=8 sw=8 noet:
#
# Cycle counts are produced by the simulated CPU model, so host
# optimisation level cannot change them — only the wall-clock cost
# of the simulator itself. -O2 + -g typically gives a 4-8x wall-clock
# speedup vs unoptimised, with full debug symbols retained.
DEBUG		= -g -O2
CXX		= g++ --std=c++20 -Wall -Wextra -Werror
CC		= gcc --std=c9x -Wall -Werror
CCFLAGS		= $(DEBUG)
# -MMD -MP makes the compiler emit a .d file beside every .o listing the
# headers it included (-MP adds phony targets so deleting a header doesn't
# break the build). These are -include'd at the foot of this file.
CPPFLAGS	= -D_POSIX_SOURCE -I. -MMD -MP
LDFLAGS		=

LIB_SRCS	= usim.cpp memory.cpp \
		  mc6809.cpp mc6809in.cpp \
		  hd6309.cpp hd6309in.cpp \
		  mc6850.cpp \
		  picotick.cpp picoide.cpp picofram.cpp \
		  system_watchpoint.cpp

OBJS		= $(LIB_SRCS:.cpp=.o)
BIN		= usim09 usim09batch usim09pt tests/test6809 tests/test6309

LIB		= libusim.a

all: $(BIN)

$(LIB): $(OBJS)
	ar crs $(@) $^
	ranlib $(@)

usim09:	$(LIB) main09.o term.o
	$(CXX) $(CCFLAGS) $(LDFLAGS) main09.o term.o -L. -lusim -o $(@)

usim02:	$(LIB) main02.o term.o
	$(CXX) $(CCFLAGS) $(LDFLAGS) main02.o term.o -L. -lusim -o $(@)

tests/test6809: $(LIB) tests/test6809.o
	$(CXX) $(CCFLAGS) $(LDFLAGS) tests/test6809.o -L. -lusim -o $(@)

tests/test6809.o: tests/test6809.cpp
	$(CXX) $(CPPFLAGS) $(CCFLAGS) -c tests/test6809.cpp -o $(@)

# rawrel writes each byte at its own address, so the image spans the
# whole space and carries the vectors at $$fff0 -- which raw does not,
# and which is why the 6309 image has to have its reset vector poked
# in by hand. Built with lwasm so the tree needs one assembler.
tests/test6809.bin: tests/test6809.asm
	lwasm --6809 --format=rawrel -o $(@) $(<)

tests/test6309: $(LIB) tests/test6309.o
	$(CXX) $(CCFLAGS) $(LDFLAGS) tests/test6309.o -L. -lusim -o $(@)

tests/test6309.o: tests/test6309.cpp
	$(CXX) $(CPPFLAGS) $(CCFLAGS) -c tests/test6309.cpp -o $(@)

# Assembled with lwtools' lwasm (6309 mode is its default). Raw format
# zero-fills the gap between the code at $0400 and the vectors at $fffe.
tests/test6309.bin: tests/test6309.asm
	lwasm --6309 --format=raw -o $(@) $(<)

.PHONY: test6309
test6309: tests/test6309 tests/test6309.bin
	./tests/test6309

usim09batch: $(LIB) main09batch.o batchterm.o
	$(CXX) $(CCFLAGS) $(LDFLAGS) main09batch.o batchterm.o -L. -lusim -o $(@)

usim09pt: $(LIB) main_picothing.o term.o ptyserial.o
	$(CXX) $(CCFLAGS) $(LDFLAGS) main_picothing.o term.o ptyserial.o -L. -lusim -o $(@)

.SUFFIXES: .cpp

.cpp.o:
	$(CXX) $(CPPFLAGS) $(CCFLAGS) -c $<

# One target: a second definition of test would override this one
# silently, which is how the CPU suites came to stop running.
.PHONY: test
test: tests/test6809 tests/test6809.bin tests/test6309 tests/test6309.bin \
      tests/test_mmu_tick tests/test_system_watchpoint tests/test_picoide
	./tests/test6809
	./tests/test6309
	./tests/test_mmu_tick
	./tests/test_system_watchpoint
	./tests/test_picoide
	@$(MAKE) --no-print-directory cycles
	@$(MAKE) --no-print-directory cycles6309

tests/test_mmu_tick: tests/test_mmu_tick.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_mmu_tick.cpp -L. -lusim -o $(@)

tests/test_system_watchpoint: tests/test_system_watchpoint.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_system_watchpoint.cpp -L. -lusim -o $(@)

tests/test_picoide: tests/test_picoide.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_picoide.cpp -L. -lusim -o $(@)

# Cycle-count regression gate.
#
# `usim09batch --timeout=1000000 --cycles samples/test_main.hex` is
# expected to report exactly EXPECTED_CYCLES on stderr. The number
# comes from the simulated CPU model (per-instruction cycle counts
# the mc6809 emits), so host compiler / optimisation level can't
# affect it — only a real behavioural change to the CPU model or
# device-tick ordering will. Any drift is a breaking change for
# downstream consumers (UniFLEX port, picolibc test harness, llvm-
# mc6809 codegen tally) that maintain their own per-test cycle
# ledgers, so it must be coordinated rather than silently absorbed.
#
# `make cycles` runs the gate standalone (exits non-zero on drift,
# prints expected vs actual); `make test` invokes it after the unit
# tests.
#
# The figure was one short of what the model has ever produced, and
# named a path that had already moved when the gate was written, so
# the gate had never once run. Both are corrected here; 3333357 is
# what every commit back to the gate's own produces.
EXPECTED_CYCLES = 3333357

.PHONY: cycles
cycles: usim09batch samples/test_main.hex
	@actual=$$(./usim09batch --timeout=1000000 --cycles samples/test_main.hex 2>&1 | sed -n 's/^cycles=//p'); \
	if [ "$$actual" = "$(EXPECTED_CYCLES)" ]; then \
		echo "cycles: OK ($(EXPECTED_CYCLES))"; \
	else \
		echo "cycles: DRIFT — expected $(EXPECTED_CYCLES), got $$actual"; \
		exit 1; \
	fi

# The same gate for the HD6309, which the one above cannot cover: it
# runs the 6809 model, so no 6309-only instruction is timed by it.
#
# The functional suite doubles as the workload, since it already
# reaches TFM, MULD, DIVD, BAND, ADDR/SUBR, SEXW and the Q and W
# moves. A mistimed 6309 instruction moves this number while every
# functional check still passes, which is exactly how TFM came to
# charge 6+5n cycles instead of 6+3n unnoticed: putting that back
# reads 2319 here.
EXPECTED_CYCLES_6309 = 2311

.PHONY: cycles6309
cycles6309: tests/test6309 tests/test6309.bin
	@actual=$$(./tests/test6309 | sed -n 's/.*cycles=//p'); \
	if [ "$$actual" = "$(EXPECTED_CYCLES_6309)" ]; then \
		echo "cycles6309: OK ($(EXPECTED_CYCLES_6309))"; \
	else \
		echo "cycles6309: DRIFT — expected $(EXPECTED_CYCLES_6309), got $$actual"; \
		exit 1; \
	fi

.PHONY: clean
clean:
	$(RM) $(BIN) $(LIB) *.o *.d tests/test_mmu_tick tests/*.o tests/*.d tests/test6809.bin tests/test_system_watchpoint tests/test_picoide

# Compiler-generated header dependencies (see CPPFLAGS -MMD -MP). Pulling
# these in means editing any header recompiles exactly the objects that
# include it — directly or transitively. This replaces the old hand-kept
# `make depend` list, which went stale: it never tracked hd6309.h / mc6809.h
# across the subclass and the three mains, so a header edit left stale
# objects and the next link produced a vtable/ABI mismatch (segfault).
-include $(wildcard *.d tests/*.d)

