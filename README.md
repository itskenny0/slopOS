# PicoOS Pro v2.1

## Pro desktop expansion

This build expands the graphical desktop while keeping the existing PicoOS kernel and application model. The new desktop features are deliberately exposed from the **Start button** rather than adding clutter to the desktop:

- Start-button launcher with program search and the first letter of each app shown in its icon.
- New bundled **Task Manager** and **Calculator** apps.
- Right-click desktop context menu with wallpaper cycling, icon arrangement, refresh and desktop settings.
- Desktop icons can be dragged and repositioned; **Arrange icons** restores the default layout.
- New **Task Manager** window in the desktop shell with live process, foreground/background and memory information.
- New **Desktop Settings** window explaining the desktop controls and storage model.
- Files window remains available from Start/Desktop and can create a text file from the background context menu.
- Existing movable, minimisable and closable windows remain intact.
- More taskbar/window affordances and live system information remain available without putting extra launch tiles on the desktop.

### Storage note

The current PicoOS kernel exposes a **RAM filesystem** to applications. The Files window therefore shows the live PicoOS filesystem and program data; it does **not** claim to enumerate physical HDD/SSD devices. A real multi-disk browser would require an ATA/AHCI/NVMe block-device driver, which is not present in this source tree. The new Settings window calls this out explicitly rather than faking disks.



See CHANGES-2.1.md for what is new: USB mouse/keyboard (xHCI, EHCI, OHCI, UHCI), a Windows/Ubuntu-style desktop, MIT licence.

---

# PicoOS Pro v2.1

This tree starts from the supplied PicoOS 1.1 source because that version
had the stable shell and boot path. The visible release is a new PicoOS
Pro 2.0 desktop: it boots straight into the modern graphical `Menu`, not
to the old 1.1/prototype desktop.

## Current interface

- modern dark dashboard with sidebar, metric cards, quick actions and an
  application grid;
- Programs and Files windows;
- clock and network status in the taskbar;
- full keyboard navigation (arrows, Tab, Enter, Esc) plus PS/2 mouse;
- icons reduced from the supplied `icons.png` sheet to the 16-colour VGA
  palette and bundled in `apps/menu_icons.h`;
- `apps/menu.c` redraws only the region that changed, so moving through
  the menu never flickers;
- the original text shell remains available from Menu and `exit` returns
  to Menu.

## Input: no setup, no waiting

USB keyboards and mice work through the firmware's PS/2 emulation, which
needs no driver and no questions. An earlier tree carried an experimental
native USB-HID driver with a countdown wizard; it never worked reliably,
so it was removed completely -- there is no countdown, no "insert the
receiver" screen, and the machine boots straight into the menu. PS/2
input keeps working exactly as before.

## DOOM

The shareware episode of DOOM (doomgeneric) is ported as `doom.pico`:

- 320x200 software rendering, integer-scaled to the screen (VBE or UEFI
  framebuffer; a 16-colour fallback screen is refused with an error);
- keyboard play with the original bindings (arrows, Ctrl, Alt, Space,
  Shift, 1-7, Esc, function keys);
- the 4 MB WAD is read in place from the disk image -- no loading wait;
- savegames and `default.cfg` persist in the RAM disk while powered on;
- from the shell: `doom -timedemo demo1` replays a demo and prints fps.

Needs a machine with ~10 MB of RAM (6 MB zone plus the WAD and the game).
There is no sound yet: PicoOS has no audio driver to play it through.
See `docs/DOOM.md` for the port notes, the controls and the test setup.

## Hardware information

A CPU identification driver uses CPUID when available and records the
vendor, brand string, family, model and stepping. The boot screen reports
the CPU and the shell command `cpu` prints detailed feature information.
The same data is available in the graphical System page through
`cpuinfo.txt`.

## Programs in this baseline

The RAM program store includes the desktop, C compiler, drawing tool,
store, text web tool, file/program browser, calculator-style utilities,
games and other programs from the 1.1 source. The intentional crash demo
and old alternative desktop implementations are not bundled.

## Build

```sh
make PRO=1
```

Only gcc and python3 are needed: the UEFI loader and the FAT images
build without gnu-efi or mtools. (The 65 MB FAT32 stick and the ISO
still want mtools/xorriso; see below.)

The Pro outputs are:

- `PicoOS-Pro-2.1.img` — BIOS floppy image;
- `PicoOS-Pro-2.1-usb-small.img` — 9 MB bootable USB stick: BIOS boot
  sector plus a FAT16 EFI partition, for machines with a legacy boot
  option. `PicoOS-USB.img` is the same file under its flashing name;
- `PicoOS-Pro-2.1.iso` — BIOS/UEFI ISO (needs xorriso).

DOOM (shareware `doom.pico` + WAD) is preinstalled on the USB images
and the ISO -- it needs `doom1.wad` next to the Makefile at build time
(or `make PRO=1 DOOM_WAD=/path/to/doom1.wad`), and shows up under APPS.

Write a `.img` to USB with `dd` or Rufus. To put DOOM on a hybrid USB
stick, move the EFI partition out of the way first -- the kernel plus
the 4 MB archive needs the room:

```sh
PICO_ESP_LBA=16384 make PRO=1 PicoOS-Pro-2.1-usb-small.img
PICO_ESP_LBA=16384 python3 tools/addapp.py PicoOS-Pro-2.1-usb-small.img \
    doom.pico=build/doom.pico DOOM1.WAD=doom1.wad
```

The build is incremental. The shell supports both `start <program>` and
`run <program>`. For example, `ls` shows the ordinary `.txt` files plus
all shipped `.pico` programs, and `run doom.pico` starts DOOM. Packed
programs are unpacked automatically only when run.

The old source tree at `/home/user/picoos-old/picoos`
is intentionally not modified.

## 2026-09-21 fixes (in every image in this zip)

- DOOM quick-access tile is labelled again: `gfx_pixel()` clipped every
  icon/text pixel at 640x480 even on a 1280x800 firmware framebuffer, so
  the third dashboard card rendered empty. The legacy clip is kept for
  VBE/mode-12h; the framebuffer path now clips at the real size
  (`PICO-FIX-gfxclip` in `kernel/gfx.c`).
- Quitting DOOM returns cleanly to the menu: keys pressed inside a
  program stayed queued and replayed into the menu after exit (arrows +
  enter once walked the sidebar to POWER and shut the machine down).
  `pico_exec()` now flushes queued and held keys before the child starts
  and after it ends (`PICO-FIX-kbdflush` in `kernel/kbd.c`,
  `kernel/exec.c`, `include/pico.h`).
- Quit flow: ESC -> QUIT GAME -> Y lands back on the dashboard with the
  DOOM tile still selected. Verified on the USB image and the ISO;
  screenshots: `screenshots/f_iso_menu.png`, `f_iso_quitback.png`.
- Note: the 1.44 MB floppy image cannot fit the 4 MB shareware WAD, so it
  ships without DOOM (2 quick tiles). USB and ISO images include DOOM.
