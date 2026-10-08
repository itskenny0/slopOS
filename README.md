# slopOS: AI provenance and release claims

**The release contains a verifiably AI-generated desktop wallpaper.** Its signed record names ChatGPT and `gpt-image`. The code also contains concrete defects and traces consistent with an assistant workflow, but the kernel’s authorship remains unresolved.

Reviewed **8 October 2026**, against upstream commit [`c8142cef`](https://github.com/PicoOS-Pro-v/picoOS/commit/c8142cef3cf7bc28906c947fab53d58385ef823a). Code citations refer to that original revision.

**Verified AI provenance**

[`docs/wallpaper-source.png`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/docs/wallpaper-source.png) contains a C2PA creation record identifying:

- Generator: `OpenAI Media Service API`
- Software: `ChatGPT`, version `gpt-image`
- Source type: `trainedAlgorithmicMedia`
- Signer: `OpenAI Media Service`, organization `OpenAI OpCo, LLC`

The signature, signing-certificate trust and image-data hashes validate against the [official C2PA trust lists](https://github.com/c2pa-org/conformance-public/tree/43a0a6f09091a062083a4ba33e3acc19dd721282/trust-list). The [verification report](docs/provenance/verification.json) records `Trusted`; it separately flags the timestamp authority as untrusted, so no independently trusted creation time is claimed.

The supplied [wallpaper converter](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/mkwallpaper.py) reproduces `apps/wallpaper.h` **byte-for-byte**, and the desktop [uses it at startup](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L1578-L1586). This establishes AI-generated content in the desktop, without identifying who prompted it or who wrote the kernel.

**What the author claimed**

In [r/osdev, responding to AI allegations](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe08rgy/), the author wrote:

> Call it AI all you want, but AI can't compile a stable custom x86 graphics driver and port Quake to a custom kernel.

In a [second reply](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe0mvek/):

> If you think this project is just AI-generated, feel free to try and build a custom x86 kernel that boots and runs Quake yourself.

Yet the author's [r/AlternativeOS announcement](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/) says:

> DOOM Port: I successfully ported `doomgeneric` as `doom.pico`.

The [game entry point](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/doom/pico/pico_main.c#L14-L24) and [build targets](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/Makefile#L253-L265) confirm DOOM. This snapshot contains no Quake port; its only Quake mentions are incidental comments in imported DOOM sources. A naming mistake or unpublished build remains possible.

A working build cannot establish human-only authorship. AI tools can invoke compilers; Anthropic has [reported an AI-written compiler building a bootable Linux kernel and running DOOM](https://www.anthropic.com/engineering/building-c-compiler). That rebuts the capability argument, without attributing PicoOS code to a particular tool.

**The implementation does not support several release claims**

The [r/AlternativeOS announcement](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/) introduces its features with:

> Here is a quick overview of what is currently implemented and stable:

It advertises:

> Custom Desktop Environment: A Windows/Ubuntu-style dark desktop interface with a Start-button launcher, application grid, live Task Manager (with process, memory, and background/foreground info), and a functional Calculator app.

| Finding | Evidence |
| --- | --- |
| **CPU graph measures a clock ratio.** | [The calculation](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L807-L820) is `(dt*1000)/(ms*10)`. Both inputs derive from [the same timer](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/timer.c#L3-L19), [initialized at 100 Hz](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/main.c#L112-L113). Consistent, non-overflowing samples yield 10%, regardless of workload. Sampling races can cause variation. |
| **Standalone Task Manager redraws stale RAM data.** | [`meminfo()` and storage sampling run only inside `if(first)`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/taskmgr.c#L8-L12); timed refreshes reuse those readings. Dragging triggers resampling. This defect concerns the standalone app. |
| **Networking is advertised but stubbed out.** | The shell [lists TCP, DHCP, DNS and HTTP](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/shell.c#L960-L968), while the missing `net/` tree causes the [build to select stubs](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/Makefile#L16-L22) that [report no interfaces and fail operations](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/stubs/net_stub.c#L12-L45). |
| **USB documentation contradicts startup.** | The [original README says native USB-HID was removed](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md#L48-L55), but [startup calls `usb_init()`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/main.c#L200-L210), which [probes all four controller types](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/usb.c#L446-L457). |
| **OHCI descriptor linking uses unallocated slots.** | The controller is zeroed, then [each descriptor takes `next` from a slot before that slot is allocated](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/ohci.c#L269-L302). All four links remain zero, breaking the intended chain. |

These are implementation and validation failures. Bugs alone cannot distinguish generated code from human mistakes.

**Circumstantial traces of an assistant workflow**

The [USB source](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/usb.c#L19-L23) refers explicitly to a request:

> Why the keyboard too, when only the mouse was asked for:

The [environment script](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/devenv.sh#L1-L49) states:

> This sandbox has no root and wipes /tmp between sessions

Tracked Python bytecode contains `/mnt/data/pico_v21/PicoOS-Pro-v2.1/tools/` paths; older archives contain `/home/user/` paths. The [original README](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md#L125-L126) also describes preserving a supplied source tree. These traces fit a hosted assistant workflow, but human collaborators and containers can produce them too.

**Release history and the author's Git explanation**

Twelve public release ZIPs were inspected. The [v2.1 archive](https://drive.google.com/file/d/1_f1yP_agk3YOhwr4VLzQAunHAiWKag4J/view) matches **311 of 312 original tracked files exactly**, including the signed image and reviewed code. Only `LICENSE` differs. Download links, hashes and embedded paths are in the [archive inventory](docs/provenance/archives.json); comparison results are in the [verification report](docs/provenance/verification.json).

Explaining the ZIP workflow in [r/AlternativeOS](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/comment/pdveaal/), the author wrote:

> I am currently using Google Drive just to quickly share the build images and the zip files while I focus 100% on the kernel development (like the new native USB drivers and the 3D graphics engine).

In [r/osdev](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe0e8id/):

> With Git, I would have to set up repositories and run a bunch of commands every time, which takes away time from writing code.

The author [later clarified](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/peeo87p/):

> no i use google drive for my iso's and github for the open source

The GitHub import confirms that update, but provides no development history before the snapshot. These workflow choices do not establish programming ability.

**Reproduce the provenance checks**

The [verification script](tools/verify_provenance.py) reads the original commit, verifies the image against pinned official trust lists, reproduces the wallpaper and reports bytecode paths:

```sh
python3 -m venv /tmp/slopos-provenance-check
/tmp/slopos-provenance-check/bin/pip install c2pa-python==0.38.0 Pillow==12.3.0 numpy==2.5.3
/tmp/slopos-provenance-check/bin/python tools/verify_provenance.py
```

Append the downloaded v2.1 ZIP's path to repeat the archive comparison.

This review and its verification script were prepared with AI assistance. Code findings are based on static inspection, not OS runtime testing. No attributable kernel-generation record was found, and the evidence does not establish the author's level of understanding.
