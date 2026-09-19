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
BIN		= usim09 usim09batch usim09pt tests/test6809 tests/test6309 \
		  tests/test_cycles6309 tests/test_picovideo

LIB		= libusim.a

# --- Pico-Thing video simulator (usim09pt only) ---
# The window renders with the pico-thing-video firmware's own PORTABLE
# renderer, compiled unchanged from that tree. SDL2 comes via pkg-config;
# -isystem keeps its headers outside -Werror's blast radius. None of this
# touches libusim or the other binaries.
PICOVIDEO	?= $(HOME)/Documents/PICO/pico-thing-video
SDL_CFLAGS	:= $(shell pkg-config --cflags sdl2 | sed 's/-I/-isystem /g')
SDL_LIBS	:= $(shell pkg-config --libs sdl2)
PV_OBJS		= picovideo.o picovideo_display.o pv_render.o pv_link.o pv_registers.o pv_console.o

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

usim09pt: $(LIB) main_picothing.o term.o ptyserial.o $(PV_OBJS)
	$(CXX) $(CCFLAGS) $(LDFLAGS) main_picothing.o term.o ptyserial.o $(PV_OBJS) $(SDL_LIBS) -L. -lusim -o $(@)

# Firmware renderer, compiled from the pico-thing-video tree (VIDEO_DEMO
# gates the built-in test pattern the simulator shows at reset).
pv_render.o: $(PICOVIDEO)/src/video_render.cpp
	$(CXX) -I$(PICOVIDEO)/include -Ipico_shim $(CPPFLAGS) $(CCFLAGS) -DVIDEO_DEMO=1 -c -o $(@) $<

picovideo.o: picovideo.cpp
	$(CXX) -I$(PICOVIDEO)/include -Ipico_shim $(CPPFLAGS) $(CCFLAGS) -DVIDEO_DEMO=1 -c -o $(@) $<

# The firmware's guest-link protocol and register window, compiled
# unchanged from the pico-thing-video tree (pico_shim no-ops the SDK
# decorations they use).
pv_link.o: $(PICOVIDEO)/src/vid_link.cpp
	$(CXX) -I$(PICOVIDEO)/include -Ipico_shim $(CPPFLAGS) $(CCFLAGS) -c -o $(@) $<

pv_registers.o: $(PICOVIDEO)/src/registers.cpp
	$(CXX) -I$(PICOVIDEO)/include -Ipico_shim $(CPPFLAGS) $(CCFLAGS) -c -o $(@) $<

pv_console.o: $(PICOVIDEO)/src/vid_console.cpp
	$(CXX) -I$(PICOVIDEO)/include -Ipico_shim $(CPPFLAGS) $(CCFLAGS) -c -o $(@) $<

picovideo_display.o: picovideo_display.cpp
	$(CXX) -I$(PICOVIDEO)/include -Ipico_shim $(CPPFLAGS) $(CCFLAGS) $(SDL_CFLAGS) -c -o $(@) $<

.SUFFIXES: .cpp

.cpp.o:
	$(CXX) $(CPPFLAGS) $(CCFLAGS) -c $<

# One target: a second definition of test would override this one
# silently, which is how the CPU suites came to stop running.
.PHONY: test
test: tests/test6809 tests/test6809.bin tests/test6309 tests/test6309.bin \
      tests/test_mmu_tick tests/test_system_watchpoint tests/test_picoide \
      tests/test_cycles6309 tests/test_picovideo
	./tests/test6809
	./tests/test6309
	./tests/test_mmu_tick
	./tests/test_system_watchpoint
	./tests/test_picoide
	./tests/test_picovideo
	./tests/test_cycles6309
	@$(MAKE) --no-print-directory cycles
	@$(MAKE) --no-print-directory cycles6309

tests/test_mmu_tick: tests/test_mmu_tick.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_mmu_tick.cpp -L. -lusim -o $(@)

tests/test_system_watchpoint: tests/test_system_watchpoint.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_system_watchpoint.cpp -L. -lusim -o $(@)

tests/test_picoide: tests/test_picoide.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_picoide.cpp -L. -lusim -o $(@)

# Links the real firmware link/renderer objects; the SDL display TU is
# replaced by stubs inside the test, so no SDL dependency here.
tests/test_picovideo: tests/test_picovideo.cpp picovideo.o pv_link.o pv_registers.o pv_render.o pv_console.o $(LIB)
	$(CXX) -I$(PICOVIDEO)/include -Ipico_shim $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_picovideo.cpp picovideo.o pv_link.o pv_registers.o pv_render.o pv_console.o -L. -lusim -o $(@)

# Per-instruction timing, both MD modes. Unlike the two aggregate gates
# below, this one names the instruction that moved, and it is the only
# thing in the tree that covers native-mode timings at all.
tests/test_cycles6309: tests/test_cycles6309.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CCFLAGS) $(LDFLAGS) tests/test_cycles6309.cpp -L. -lusim -o $(@)

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
# reads 2304 here.
#
# It moved from 2311 to 2296 when MULD, DIVD and DIVQ stopped adding
# their whole documented figure on top of the operand fetches they
# had already charged. That is the gate working: a deliberate timing
# correction has to be acknowledged here before it can land.
#
# 2296 -> 2222: the HD6309 instructions were reconciled against Darren
# Atkinson's 6x09 reference and Chet Simpson's HD6309 Technical
# Reference Guide, opcode by opcode and postbyte by postbyte. Several
# groups were over-charging in emulation mode -- the $104x/$105x 16-bit
# inherent ops, the BAND..STBT bit group, LDMD, BITMD, SEXW, DIVQ,
# OIM/AIM/EIM/TIM, and the E,R / F,R / W,R / n16,W indexed postbytes --
# because each added its whole documented figure on top of bytes the
# fetch had already charged. ABX and CWAI were under-charging in the
# shared MC6809 code and were corrected too; neither is executed by the
# 6809 workload above, which is why EXPECTED_CYCLES did not move.
#
# Every one of the 430 documented opcodes and 249 indexed postbytes now
# matches the reference exactly in BOTH MD modes. That is asserted
# per-instruction by tests/test_cycles6309, which is the gate that
# actually protects this work; this aggregate figure is a backstop.
#
# 2222 -> 2408: test 21 was added to the suite, a block that runs the
# same instruction sequence once in native mode and once in emulation
# mode. Before it, this gate was almost blind to native timings -- the
# only native-mode instructions in the suite were a SWI/RTI pair whose
# cost the bus model already got right. Assembling that block with
# `ldmd #0` instead of `ldmd #1` reads 2426, so the 18-cycle gap is
# what this figure now protects.
#
# 2408 -> 2392: the illegal-instruction / divide-by-zero trap was
# charging 12 on top of its stacking where SWI2 and SWI3, whose entry
# sequence it shares, charge 4. That made a trap cost 28 against their
# 20 for the identical hardware sequence. The suite takes two traps
# (test 13's divide by zero, test 14's illegal instruction), so the
# figure drops by exactly 16.
EXPECTED_CYCLES_6309 = 2392

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
	$(RM) $(BIN) $(LIB) *.o *.d tests/test_mmu_tick tests/*.o tests/*.d tests/test6809.bin tests/test_system_watchpoint tests/test_picoide tests/test_cycles6309 tests/test_picovideo

# Compiler-generated header dependencies (see CPPFLAGS -MMD -MP). Pulling
# these in means editing any header recompiles exactly the objects that
# include it — directly or transitively. This replaces the old hand-kept
# `make depend` list, which went stale: it never tracked hd6309.h / mc6809.h
# across the subclass and the three mains, so a header edit left stale
# objects and the next link produced a vtable/ABI mismatch (segfault).
-include $(wildcard *.d tests/*.d)

