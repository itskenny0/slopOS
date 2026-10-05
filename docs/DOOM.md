# DOOM on PicoOS

The shareware episode of DOOM, ported via
[doomgeneric](https://github.com/ozkl/doomgeneric) as `apps/doom`,
built to `build/doom.pico` and shipped on `PicoOS-Pro-2.0-doom.img`
together with the WAD.

## Playing

From the graphical menu pick the DOOM card (or the red DOOM tile on the
Applications page). From the text shell:

```sh
run doom.pico                # play
doom -timedemo demo1         # benchmark: replays demo1, prints gametics + fps
doom -episode 2 -skill 4     # start deeper in, harder
```

Quitting: the in-game menu (Esc) offers "Quit Game", which returns to
whatever started DOOM. Ctrl-C also works as an emergency exit.

### Controls (original bindings)

| Key | Action |
|---|---|
| Arrows | walk / turn |
| Ctrl | fire |
| Space | use / open |
| Alt | strafe (hold with Left/Right) |
| Shift | run (hold) |
| `,` / `.` | strafe left / right |
| 1-7 | select weapon |
| Tab | automap |
| Esc | menu |
| Enter | confirm |
| F1-F10 | help, save, load, sound volume, gamma, ... |
| Y / N | answer yes/no prompts |

## What this machine needs

- a 256-colour screen: VBE on BIOS boot, or any UEFI framebuffer. On the
  16-colour planar fallback DOOM refuses to start with an error;
- about 10 MB of RAM: 6 MB zone, the 4 MB WAD mapped in place, the game
  itself (~600 KB) and stack/heap headroom. Too little RAM ends in a
  clean "Unable to allocate ... for zone" error, not a crash.

There is no sound: PicoOS has no audio driver yet. Savegames and
`default.cfg` live in the RAM disk, so they last until power-off.

## How the port works

- `apps/doom/src/` -- 79 upstream doomgeneric files, unmodified, built
  with `-DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200`;
- `apps/doom/pico/doomgeneric_pico.c` -- the platform layer: palette +
  frame handoff to the kernel, scancode polling with edge detection;
- `apps/doom/pico/pico_libc.c` -- the C library DOOM thinks it has:
  malloc over `api->malloc`, strings, a tiny printf, a five-conversion
  sscanf, and whole-file stdio over the ramdisk (reads are zero-copy
  through `fsmap`, writes land on fclose);
- `apps/doom/pico/pico_mfixed.c` -- FixedMul/FixedDiv in 386 assembly
  (one `imull`/`idiv` each), since `-nostdlib` has no 64-bit helpers;
- `apps/doom/pico/pico_isystem.c` -- upstream i_system.c with two
  changes: quitting exits to the OS, and errors drop to text mode
  before printing so the message is readable;
- `apps/doom/libc/` -- freestanding headers for the above.

The kernel grew five api calls for this (api v13, see `picoapp.h`):
`gfx256`, `setpal`, `blit`, `fsmap`, `keydown`. It also accepts programs
up to 1 MB (was 64 KB), PAPP archives up to 12 MB (was 4 MB), 32 KB task
stacks (was 8 KB), and always restores the text screen when a graphics
program exits or crashes. The WAD and `doom.pico` ride behind the kernel
in a PAPP archive; `tools/mkhd.py` builds that disk image.

## Testing without hardware

`tools/doomtest/` runs the exact same Doom sources and pico layer on a
64-bit Linux box against a syscall-only fake kernel (no libc): WAD
preloaded, frames dumped as PPM, config writes captured. It replays all
three shareware demos and runs the attract loop:

```sh
cd tools/doomtest && ./build.sh && ./doomtest -timedemo demo1
```

`tools/doomtest/asm32/` validates the 386 FixedMul/FixedDiv against
20000 precomputed vectors (needs 32-bit execution, `gcc -m32`).

Both harnesses passed here: timedemos of demo1/2/3 complete
(5000+/3800+/2100+ frames), the renderer output matches PC DOOM, and
the fixed-point assembly is bit-exact.

## License

DOOM is GPL (see `apps/doom/LICENSE`); the files in `apps/doom/pico/`
and `apps/doom/libc/` are GPL-2.0-or-later as part of this port.
