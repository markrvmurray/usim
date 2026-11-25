# USIM

A framework for emulating 8-bit processors, written in C++.

The currently supported processors are:
- Motorola MC6809
- Hitachi HD6309, in both emulation and native mode
- MOS 6502 (NMOS version)

The primary repository for the code is
https://github.com/raybellis/usim/.

The code requires a C++20 compatible compiler.

## History

This code was first started in 1993 and was specifically for the Motorola
6809 processor.  The initial released versions had some serious known bugs
in the handling of some 6809 instructions and addressing modes.  Many
thanks are due to B. Armstrong and Soren Roug for finding and resolving
those.

As of January 2021 support for interrupts was implemented, and
the code was reorganised so that complete machines are generated
by creating instances of a CPU, memory devices and peripherals and then
"attaching" those to each other.  See main09.cpp for an example of how
this is done.

(In earler versions it was necessary to create a subclass of the CPU
object and overload the `read` and `write` functions to direct memory
and I/O accesses accordingly).

While this framework was always intended to be extensible for other CPU
types, this didn't happen until December 2025 when MOS 6502 support was
added.

## This fork

This is Mark Murray's fork (https://github.com/markrvmurray/usim/,
branch `pico-thing`). It adds an emulator for the Pico-Thing, a 6809
single-board computer, and the tools that grew up around it: see
[README-pico-thing.md](README-pico-thing.md).

Beyond the Pico-Thing, it adds:

- **HD6309 support** (`--hd6309`, with `--md-native` to start in native
  mode): the full instruction set, native-mode interrupt stacking, the
  illegal-instruction and divide-by-zero traps, and cycle counts in both
  modes that match Darren Atkinson's real-silicon reference.
- **A GDB remote stub** (`--gdb[=PORT]`) for live debugging with lldb or
  gdb: breakpoints, watchpoints on data accesses, and monitor commands.
  Reading a device's registers from the debugger does not disturb it.
- **ELF32 loading**, alongside Intel HEX and Motorola S-records.
- **6809 behaviour fixes**: TFR and EXG with an undefined register, and a
  `$10`/`$11` prefix before an undefined opcode, run as on a real 6809
  instead of stopping the emulator; stdin reaching EOF no longer floods
  the ACIA with `$FF`.

The fork builds three 6809 emulators. The 6502 build is turned off.

| Program | What |
|---|---|
| `usim09` | an interactive 6809 machine: 32 KB RAM, 8 KB ROM, and an ACIA at `$FFC4` on the terminal |
| `usim09batch` | a 6809 machine for scripted tests: RAM throughout, an ACIA at `$FFC4`, and an exit device at `$FFCC` (the value written is the exit status) |
| `usim09pt` | the Pico-Thing; see [README-pico-thing.md](README-pico-thing.md) |

The I/O sits at the Pico-Thing's `$FFCx` addresses in all three, so one
debug build of a firmware runs on any of them.

### Building and testing

```sh
make
make test
```

`make test` runs the 6809 and 6309 functional suites, exhaustive
instruction checks, the device and debugger tests, and two cycle-count
gates. Building `usim09pt` needs SDL2 and the Pico-Thing video firmware
sources; see [README-pico-thing.md](README-pico-thing.md#building).
