# PicoOS: your own files undermine your AI defense

You defended the project against AI allegations with this [claim](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe08rgy/):

> Call it AI all you want, but AI can't compile a stable custom x86 graphics driver and port Quake to a custom kernel.

**That defense leaves out verified AI-generated content and substitutes a working demo for evidence of authorship.** Your release ships a signed ChatGPT-generated wallpaper. Your replies invoke a Quake port that the published source does not contain. Your advertised Task Manager includes a CPU graph that does not measure CPU use. These are checkable problems in your own release.

Reviewed **8 October 2026**, against upstream commit [`c8142cef`](https://github.com/PicoOS-Pro-v/picoOS/commit/c8142cef3cf7bc28906c947fab53d58385ef823a). All code links point to that original revision.

**Your desktop ships AI-generated material. The file names the tool.**

[`docs/wallpaper-source.png`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/docs/wallpaper-source.png) contains a signed C2PA creation record:

| Field | Value |
| --- | --- |
| Generator | `OpenAI Media Service API` |
| Software agent | `ChatGPT`, version `gpt-image` |
| Source type | `trainedAlgorithmicMedia` |
| Signing identity | `OpenAI Media Service`, organization `OpenAI OpCo, LLC` |

The signature, signing-certificate trust and image-data hashes validate against the [official C2PA trust lists](https://github.com/c2pa-org/conformance-public/tree/43a0a6f09091a062083a4ba33e3acc19dd721282/trust-list). The [verification report](docs/provenance/verification.json) records `Trusted`. Timestamp trust is separately flagged as untrusted; no independently trusted creation time is claimed.

The committed [converter](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/mkwallpaper.py) reproduces [`apps/wallpaper.h`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/wallpaper.h) **byte-for-byte**, and the desktop [includes that data](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L19-L20) and [uses it at startup](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L1578-L1586). This is an AI-generated asset incorporated into the desktop. A defense of the project's provenance that leaves it out is incomplete. It does not, by itself, attribute the kernel to AI.

**You repeatedly invoke Quake. The supplied port is DOOM.**

In a [second reply](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe0mvek/), you wrote:

> If you think this project is just AI-generated, feel free to try and build a custom x86 kernel that boots and runs Quake yourself.

Your own [r/AlternativeOS announcement](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/) says:

> DOOM Port: I successfully ported `doomgeneric` as `doom.pico`.

The [entry point](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/doom/pico/pico_main.c#L14-L24) calls `doomgeneric_Create()` and `doomgeneric_Tick()`. The [build packages](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/Makefile#L253-L265) `doom.pico` and `doom1.wad`. The only Quake mentions are [two](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/doom/src/d_iwad.c#L378) [incidental comments](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/doom/src/z_zone.h#L15-L20) in imported DOOM sources.

The published evidence supports a DOOM port. It does not support the Quake achievement repeatedly offered as a rebuttal. An unpublished build or naming mistake would need explaining; repeating the claim supplies neither.

The project also [credits doomgeneric](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/docs/DOOM.md#L50-L65). The bundled engine is upstream work, not evidence that either you or an AI wrote every line.

**A working build does not establish who wrote the code.**

Your [Makefile uses GCC](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/Makefile#L1-L61). GCC accepts source regardless of whether a person, a model, or both produced it.

AI coding tools can also operate compilers and tests: Anthropic [documented those command-line capabilities](https://www.anthropic.com/news/claude-3-7-sonnet) and later [reported an AI-written compiler building a bootable Linux kernel and running DOOM](https://www.anthropic.com/engineering/building-c-compiler). That experiment involved human-built infrastructure and retained GCC for 16-bit x86 boot code. It still defeats the premise that working low-level software excludes AI assistance. It does not identify PicoOS's code author.

**Your “live” Task Manager does not measure what it displays.**

The [announcement](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/) advertises:

> Custom Desktop Environment: A Windows/Ubuntu-style dark desktop interface with a Start-button launcher, application grid, live Task Manager (with process, memory, and background/foreground info), and a functional Calculator app.

The desktop [labels its graph CPU](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L829-L847), but [calculates](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L807-L820):

```c
int cpu=(ms && dt)?(int)((dt*1000)/(ms*10)):0;
```

`dt` is elapsed ticks; `ms` is elapsed milliseconds. The [API wrappers](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/exec.c#L43-L46) read [the same timer](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/timer.c#L3-L19), [initialized at 100 Hz](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/main.c#L112-L113). For consistent samples without overflow, `ms = 10 * dt`, so the result is **10%, regardless of workload**. Sampling races can produce variation; CPU work is never an input.

The kernel already [accounts for running tasks separately from waiting tasks](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/task.c#L134-L142) and [exports per-task CPU ticks](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/task.c#L437-L453). The desktop ignores that accounting. A refreshing graph does not make the reported statistic meaningful.

The [standalone Task Manager](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/taskmgr.c#L8-L12) has a separate defect: `meminfo()` and storage sampling run inside `if(first)`, then `first` becomes zero. Timed redraws reuse those readings and append stale RAM samples. Dragging triggers resampling; ordinary refreshes do not. The integrated desktop does re-query memory, so this RAM defect is specific to the standalone app.

These failures expose a gap between a feature looking complete and actually working as described.

**Your stability and feature claims outrun the implementation.**

The same [announcement](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/) states:

> Here is a quick overview of what is currently implemented and stable:
>
> Native USB Stack (NEW): Full native driver support for USB 1.1, 2.0, and 3.0 (UHCI/OHCI, EHCI, and xHCI controllers), handling mice and keyboards smoothly without relying on firmware emulation.

- **USB documentation contradicts startup.** The [original README says native USB-HID was removed](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md#L48-L55), while [startup calls `usb_init()`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/main.c#L200-L210), which [probes all four controller types](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/usb.c#L446-L457). Those descriptions cannot both accurately describe this revision.
- **The shell advertises an absent network stack.** Its [version banner lists TCP, DHCP, DNS and HTTP](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/shell.c#L960-L968). There is no `net/` tree, so the [build selects stubs](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/Makefile#L16-L22) that [report zero interfaces and fail network operations](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/stubs/net_stub.c#L12-L45). The banner promises functionality this build cannot supply.
- **OHCI links descriptors before allocating their targets.** After [zeroing the controller](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/ohci.c#L269-L272), the [allocation loop](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/ohci.c#L295-L302) reads each next slot before allocating it. All four `next` links remain zero, and every interrupt-table entry points to the first descriptor. The [promised chain covering every endpoint](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/ohci.c#L3-L11) is not constructed.

These findings undermine the release's validation claims. They do not establish that every machine fails, or that humans could not have made the mistakes.

**The source records requests, a temporary sandbox and source handoffs.**

The [USB implementation](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/usb.c#L19-L23) explains why it exceeded a request's scope:

> Why the keyboard too, when only the mouse was asked for:

The [environment setup script](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/devenv.sh#L1-L49) says:

> This sandbox has no root and wipes /tmp between sessions

It downloads and extracts Debian packages into a temporary prefix. The [QEMU helper](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/screen.py#L18-L20) expects tools under `/tmp/qroot`, while the [screenshot converter](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/ppm2png.py#L1-L8) explains that the sandbox lacks image tools and PIL. The environment description is reflected in the tooling itself.

The original README [describes supplied PicoOS 1.1 sources](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md#L29-L32), then [records](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md#L125-L126):

> The old source tree at `/home/user/picoos-old/picoos`
> is intentionally not modified.

The [network stub's introduction](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/stubs/net_stub.c#L1-L6) likewise tells its recipient that a supplied ZIP lacked network sources and explains how to restore them.

Tracked Python bytecode preserves these source-path strings:

```text
/mnt/data/pico_v21/PicoOS-Pro-v2.1/tools/lzss.py
/mnt/data/pico_v21/PicoOS-Pro-v2.1/tools/mkesp.py
```

[Earlier archives](docs/provenance/archives.json) contain `/home/user/picoos/` paths; v2.0 contains `/home/user/myos/PicoOS-Pro-v2.0/tools/`. Request explanations, restricted tooling and archive-handoff language together fit an assistant editing supplied releases. This is a circumstantial interpretation: human collaborators and containers can leave the same traces, and directory names do not identify an AI vendor.

**The Git explanation does not supply the missing development history.**

In the [same AI-defense comment](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe08rgy/), you wrote:

> I know how version control works, I just chose not to use it because I wanted to spend 100% of my time inside the actual kernel code and drivers instead of managing repositories.

In [r/AlternativeOS](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/comment/pdveaal/):

> I am currently using Google Drive just to quickly share the build images and the zip files while I focus 100% on the kernel development (like the new native USB drivers and the 3D graphics engine).

In [r/osdev](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe0e8id/):

> With Git, I would have to set up repositories and run a bunch of commands every time, which takes away time from writing code.

These comments explain a distribution preference. They do not document how the code was produced. Twelve public ZIPs were inspected; the [v2.1 archive](https://drive.google.com/file/d/1_f1yP_agk3YOhwr4VLzQAunHAiWKag4J/view) matches **311 of 312 original tracked files byte-for-byte**, including the signed image and reviewed code. Only `LICENSE` differs. The [inventory](docs/provenance/archives.json) and [comparison report](docs/provenance/verification.json) preserve the evidence. The v0.6 ZIP even contains initialized `.git` metadata, but no object or ref files.

You [subsequently stated](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/peeo87p/):

> no i use google drive for my iso's and github for the open source

The GitHub import confirms that update. Its single initial commit supplies a snapshot, not a development record. Git unfamiliarity is not proof of AI authorship; the absence of attributable development history leaves that question unanswered.

**Check the evidence yourself.**

The [verification script](tools/verify_provenance.py) reads the original commit, verifies the image against pinned official trust lists, reproduces the wallpaper and reports bytecode paths:

```sh
python3 -m venv /tmp/slopos-provenance-check
/tmp/slopos-provenance-check/bin/pip install c2pa-python==0.38.0 Pillow==12.3.0 numpy==2.5.3
/tmp/slopos-provenance-check/bin/python tools/verify_provenance.py
# Append the downloaded v2.1 ZIP's path to repeat the archive comparison.
```

The [announcement](https://www.reddit.com/r/osdev/comments/1wxom7n/picoos_pro_v21_my_custom_32bit_x86_os_written/) defines “from scratch” as no Linux kernel or external base; it does not explicitly declare that no AI was ever used. This rebuttal addresses the quoted defense and published functionality. It establishes AI-generated image content and specific implementation failures, not deliberate deception or complete AI authorship.

This review and its verification script were prepared with AI assistance. Code findings are based on static inspection, not OS runtime testing. No attributable kernel-generation record was found.
