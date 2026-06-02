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
CPPFLAGS	= -D_POSIX_SOURCE -I.
LDFLAGS		=

LIB_SRCS	= usim.cpp memory.cpp \
		  mc6809.cpp mc6809in.cpp \
		  mc6850.cpp \
		  picotick.cpp picoide.cpp picofram.cpp \
		  system_watchpoint.cpp

OBJS		= $(LIB_SRCS:.cpp=.o)
BIN		= usim09 usim09batch usim09pt tests/test6809

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

tests/test6809.bin: tests/test6809.asm
	asm6809 -B -o $(@) $(<)

.PHONY: test
test: tests/test6809 tests/test6809.bin
	tests/test6809

usim09batch: $(LIB) main09batch.o batchterm.o
	$(CXX) $(CCFLAGS) $(LDFLAGS) main09batch.o batchterm.o -L. -lusim -o $(@)

usim09pt: $(LIB) main_picothing.o term.o
	$(CXX) $(CCFLAGS) $(LDFLAGS) main_picothing.o term.o -L. -lusim -o $(@)

.SUFFIXES: .cpp

.cpp.o:
	$(CXX) $(CPPFLAGS) $(CCFLAGS) -c $<

.PHONY: test
test: tests/test_mmu_tick tests/test_system_watchpoint tests/test_picoide
	./tests/test_mmu_tick
	./tests/test_system_watchpoint
	./tests/test_picoide
	@$(MAKE) --no-print-directory cycles

tests/test_mmu_tick: tests/test_mmu_tick.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_mmu_tick.cpp -L. -lusim -o $(@)

tests/test_system_watchpoint: tests/test_system_watchpoint.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_system_watchpoint.cpp -L. -lusim -o $(@)

tests/test_picoide: tests/test_picoide.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_picoide.cpp -L. -lusim -o $(@)

# Cycle-count regression gate.
#
# `usim09batch --timeout=1000000 --cycles tests/test_main.hex` is
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
EXPECTED_CYCLES = 3333356

.PHONY: cycles
cycles: usim09batch tests/test_main.hex
	@actual=$$(./usim09batch --timeout=1000000 --cycles tests/test_main.hex 2>&1 | sed -n 's/^cycles=//p'); \
	if [ "$$actual" = "$(EXPECTED_CYCLES)" ]; then \
		echo "cycles: OK ($(EXPECTED_CYCLES))"; \
	else \
		echo "cycles: DRIFT — expected $(EXPECTED_CYCLES), got $$actual"; \
		exit 1; \
	fi

.PHONY: clean
clean:
	$(RM) $(BIN) $(LIB) *.o tests/test_mmu_tick tests/*.o tests/test6809.bin tests/test_system_watchpoint tests/test_picoide

.PHONY: depend
depend:
	makedepend 	$(LIB_SRCS) main.cpp term.cpp

# Manually defined dependencies

usim.o: usim.h device.h typedefs.h memory.h wiring.h
usim.o: bits.h
mc6809.o: mc6809.h wiring.h usim.h device.h typedefs.h
mc6809.o: memory.h bits.h
mc6809in.o: mc6809.h wiring.h usim.h device.h typedefs.h
mc6809in.o: memory.h bits.h
mc6850.o: mc6850.h device.h typedefs.h wiring.h bits.h
memory.o: memory.h device.h typedefs.h
picotick.o: picotick.h device.h typedefs.h wiring.h bits.h
picoide.o: picoide.h device.h typedefs.h
picofram.o: picofram.h device.h typedefs.h
system_watchpoint.o: system_watchpoint.h device.h typedefs.h wiring.h mc6809.h
main.o: mc6809.h wiring.h usim.h device.h
main.o: typedefs.h memory.h bits.h mc6850.h
main.o: term.h
main09.o: mc6809.h wiring.h usim.h device.h
main09.o: typedefs.h memory.h bits.h mc6850.h
main09.o: term.h system_watchpoint.h
term.o: term.h mc6850.h device.h typedefs.h wiring.h
batchterm.o: batchterm.h mc6850.h device.h typedefs.h wiring.h
main09batch.o: mc6809.h wiring.h usim.h device.h
main09batch.o: typedefs.h memory.h bits.h mc6850.h
main09batch.o: batchterm.h haltdev.h system_watchpoint.h
main_picothing.o: mc6809.h wiring.h usim.h device.h
main_picothing.o: typedefs.h memory.h bits.h mc6850.h
main_picothing.o: term.h picotask.h picotick.h picoide.h picofram.h
main_picothing.o: tracectl.h system_watchpoint.h

# DO NOT DELETE THIS LINE -- make depend depends on it.

