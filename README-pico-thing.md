# usim09pt: the Pico-Thing emulator

`usim09pt` emulates the Pico-Thing, a 6809 single-board computer: a
real MC6809 (or HD6309) whose RAM, memory management and I/O are
provided by an RP2350 companion, with a PATA IDE bus and a second
RP2350 driving DVI video and a USB keyboard. It boots the same
firmware and disk images as the board, and runs NitrOS-9 Level 2 and
UniFLEX.

See [README.md](README.md) for the framework as a whole.

## Building

```sh
make usim09pt
make test
```

As well as a C++20 compiler, `usim09pt` needs:

- **SDL2**, found through `pkg-config` (`brew install sdl2`).
- **The `pico-thing-video` firmware sources.** The video device is drawn by
  the video firmware's own renderer, compiled unchanged from that tree. The
  Makefile looks for it at `~/GitHub/PICO/pico-thing-video`; set
  `PICOVIDEO=/path/to/it` to point elsewhere. The sources are at
  https://github.com/markrvmurray/pico-thing-video.

`usim09` and `usim09batch` need neither.

## Memory map

`$0000` is at the top of memory and `$FFFF` at the bottom.

| Address | What |
|---|---|
| `$0000-$DFFF` | RAM through the DAT: 7 pages of 8 KB, translated by the running task's DAT entries. An entry of `$FF` marks a page unavailable, and any access to it raises NMI (reads give `$FF`, writes are dropped). |
| `$E000-$FDFF` | RAM never remapped, fixed to physical `$1FE000-$1FFDFF` |
| `$FE00-$FEFF` | DAT RAM: 32 tasks × 8 pages |
| `$FF00-$FF09` | PATA IDE (the ATA registers, one address on) |
| `$FF10-$FF17` | The video Pico's guest link |
| `$FF32-$FF33` | DS12885 real-time clock (address, data) |
| `$FFC0` | DAT task register |
| `$FFC4-$FFC5` | Console ACIA (MC6850) |
| `$FFC6-$FFC7` | Auxiliary ACIA (MC6850) |
| `$FFC8-$FFC9` | 50 Hz tick timer |
| `$FFCA` | System watchpoint control |
| `$FFCB` | Trace control (emulator only: the board has nothing here) |
| `$FFD0-$FFDF` | FRAM, kept in a file between runs |
| `$FFE0-$FFFF` | System watchpoint shadow, holding the vectors at `$FFF0-$FFFF` |

Both ACIAs and the tick timer share one IRQ line. NMI comes from the
system watchpoint and from a DAT bad-page access. FIRQ is unused.

## Running

```sh
usim09pt [options] FIRMWARE
```

The firmware may be Intel HEX, Motorola S-records or ELF32. `usim09pt
--help` lists every option; the main ones are below.

### Disks, clock and FRAM

| Option | |
|---|---|
| `-d DISK` | IDE master image |
| `-D DISK` | IDE slave image (must exist; without it the bus has one drive) |
| `-f FILE` | FRAM file (default `picothing.fram`) |
| `-r FILE` | RTC file (default `picothing.rtc`); the clock follows the host's local time |
| `--rtc-time=YYYY-MM-DDTHH:MM:SS` | start the clock here and run it on guest cycles, with no file, so runs repeat exactly |

### The consoles

By default the console ACIA is the emulator's own terminal; type
`<Enter>~.` to quit, as with ssh. The auxiliary ACIA is connected to
nothing.

| Option | |
|---|---|
| `--console-pty[=PATH]` | put the console on a host pseudo-terminal instead |
| `--aux-pty[=PATH]` | put the auxiliary ACIA on a host pseudo-terminal, for DriveWire or a serial tool |
| `--video[=SCALE]` | open the video Pico's screen in a window, with its keyboard |
| `--screenshot=FILE` | save the video screen as a PNG at exit; no window needed |

A pseudo-terminal's device name (`/dev/ttysNNN`) is printed at startup.
With `=PATH`, PATH is also made a symlink to it for the run, so a
terminal program can be pointed at one name and find every run there.
Another emulator that is still running keeps its PATH; a link left by
one that died is replaced.

The link is raw 8-bit, with no baud rate: bytes move as fast as both
sides go.

### Holding the guest until you're ready

`--prompt` (with `--console-pty`) puts a `usim09pt>` prompt on the
emulator's own terminal and holds the guest at reset until you type
`run`. It is the way to attach a terminal before the guest's first
byte:

```sh
usim09pt --console-pty=/tmp/usim-console --prompt -d boot.dsk firmware.srec
# attach the terminal program to /tmp/usim-console, then:
usim09pt> run
```

The commands are `run`, `stop`, `step [N]`, `regs`, `mem ADDR [LEN]`,
`break`/`delete`, `reset`, `quit` and the debugger's monitor commands;
`help` lists them. Ctrl-C stops the guest. `gdb [PORT]` hands the CPU
to a debugger until it detaches.

### Scripted runs

For tests, the console can be fed from the command line, with each byte
released at a fixed guest cycle so that a run repeats exactly:

```sh
usim09pt --timeout=300000000 --input-first=78000000 \
    --input=$'\r\r\rdir\r' -d boot.dsk firmware.srec > run.log
```

`--video-input` does the same for the video Pico's keyboard.
`--timeout=N` stops after N instructions with exit status 124.

### Debugging

| Option | |
|---|---|
| `--gdb[=PORT]` | wait for lldb or gdb on localhost (default port 2159) and run under it |
| `--trace`, `--trace-from=N`, `--trace-to=N` | instruction trace, optionally windowed |
| `--brk=ADDR,...` | dump the registers before each listed PC |
| `--watch=[p:]ADDR` | report every change to a byte |
| `--wlog=[p:]ADDR[,LEN]` | log every store into a range, without disturbing timing |
| `--dump=[p:]ADDR[,LEN]` | hex dump at each `--brk` and at exit |
| `--pc-hist[=N]` | the N hottest code ranges at exit |
| `--trap-sled` | stop at the first jump onto a run of `$00` bytes |

A `p:` prefix gives a physical address, bypassing the DAT. Under
lldb or gdb, `monitor help` lists the DAT commands (`task`, `dat`,
`phys`). The debugger must understand the MC6809; Apple's lldb does
not, so use the one built from llvm-mc6809.

`--guest=uniflex` turns on diagnostics that know UniFLEX's kernel
layout; `usim09pt --help` lists them. They give false alarms under
any other guest, so they are off by default.

### The CPU

`--hd6309` runs an HD6309 instead of an MC6809; `--md-native` starts it
in native mode.

## Caveats

- **No baud pacing** on the ACIAs: a pseudo-terminal runs at host speed.
- **The tick timer runs on guest cycles** (`--tick=N`, default 20000 for
  50 Hz at 1 MHz), not on host time.
- **Some behaviour differs from MAME's**, deliberately, where real-silicon
  measurements disagree with it. The `$10`/`$11` prefix before an
  undefined opcode follows Darren Atkinson's measurements, unconfirmed on
  a real MC6809.
