# slopOS: documented AI provenance and problems in the release

**This release includes an AI-generated desktop wallpaper with verifiable OpenAI provenance.** Its embedded, cryptographically verified C2PA record identifies `ChatGPT` and `gpt-image`. The supplied converter reproduces the committed desktop wallpaper data byte-for-byte from that image. This establishes AI-generated material in the project; it does not establish who wrote the kernel.

There is also a concrete case against the release's validation: its desktop CPU graph does not measure CPU use, its standalone Task Manager repeatedly displays stale memory readings, its shell advertises an absent network stack, and its author's defense invokes a Quake port that this snapshot does not substantiate. Archived build artifacts and request-oriented comments provide further, circumstantial evidence of an assistant-style development workflow. The findings below distinguish direct provenance, implementation defects and inference.

Reviewed on **8 October 2026**, against commit [`c8142cef3cf7bc28906c947fab53d58385ef823a`](https://github.com/PicoOS-Pro-v/picoOS/commit/c8142cef3cf7bc28906c947fab53d58385ef823a), the upstream code revision from which this fork was cloned. Repository citations below point to that fixed revision, including the [original README](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md).

**What the author said across Reddit**

The author posted the release in both [r/osdev](https://www.reddit.com/r/osdev/comments/1wxom7n/picoos_pro_v21_my_custom_32bit_x86_os_written/) and [r/AlternativeOS](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/). Both announcements contain this statement:

> I wanted to share my hobby operating system that I’ve been building completely from scratch (no Linux kernel, no external base).

The parenthetical defines the claim in terms of the OS base; it does not specify whether AI tools were used. The excerpts below were checked against public posts and comments by `u/picoOS-official` on 8 October 2026, preserving their wording. An [earlier r/osdev submission](https://www.reddit.com/r/osdev/comments/1wxkqw5/picoospro_v21/) is listed as removed, so its unavailable body is not used as evidence.

In a [reply challenging the AI allegations](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe08rgy/), the author wrote these consecutive sentences:

> I know how version control works, I just chose not to use it because I wanted to spend 100% of my time inside the actual kernel code and drivers instead of managing repositories. Call it AI all you want, but AI can't compile a stable custom x86 graphics driver and port Quake to a custom kernel.

That is the argument addressed here. It should not be silently rewritten into a stronger quotation such as an explicit declaration that no AI was ever used.

**Direct provenance: the desktop wallpaper's signed record names ChatGPT**

The original commit includes [`docs/wallpaper-source.png`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/docs/wallpaper-source.png). Its SHA-256 is `ea13e5fa67efa87a86cfbdf4415e9b2ff44ec95ee2221a8db78b4b1eccf16355`. The PNG contains a C2PA manifest with these fields:

| Field | Recorded value |
| --- | --- |
| Claim generator | `OpenAI Media Service API` |
| Action | `c2pa.created` |
| Software agent | `ChatGPT`, version `gpt-image` |
| Digital source type | `trainedAlgorithmicMedia` |
| Recorded creation time | `2026-10-01T16:48:51.791839430Z` |
| Signing certificate identity | `OpenAI Media Service`, organization `OpenAI OpCo, LLC` |

These are authenticated fields, not a judgment about the image's appearance. Using `c2pa-python 0.38.0` / SDK `0.91.0` with the [official C2PA trust lists at a pinned revision](https://github.com/c2pa-org/conformance-public/tree/43a0a6f09091a062083a4ba33e3acc19dd721282/trust-list) returns `Trusted`, including successful signature, signing-certificate trust and image-data hash checks. The [complete verification report](docs/provenance/verification.json) preserves all results and settings. It also preserves an informational `timeStamp.untrusted` result: the timestamp digest validates, but the timestamp authority's trust chain was not established. The creation time above is therefore reported as recorded, without claiming an independently trusted timestamp. Certificate-purpose settings follow the [C2PA trust model](https://spec.c2pa.org/specifications/specifications/2.3/specs/C2PA_Specification.html#_trust_model); no certificate allow-list or disabled verification was used.

The connection to the actual desktop is reproducible. Running the committed [`tools/mkwallpaper.py`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/mkwallpaper.py) with its default settings produces exactly the committed [`apps/wallpaper.h`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/wallpaper.h). The desktop [includes that header](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L19-L20) and [builds the wallpaper at startup](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L1578-L1586).

**Conclusion supported by this artifact:** AI-generated image content is incorporated into the desktop. The record does not identify the person who prompted ChatGPT, reveal that prompt, or attribute any C or assembly code to an AI.

**Release provenance: the public ZIP matches the GitHub import**

The [public release folder](https://drive.google.com/drive/folders/19LPX5pHZpIYsnQT0Tiatpv11nE39frHA) supplies an earlier record than the single GitHub import. Twelve ZIPs, from v0.1 through v2.1, were downloaded and inspected; their URLs, sizes, SHA-256 hashes and embedded bytecode paths are preserved in the [archive inventory](docs/provenance/archives.json).

The [v2.1 ZIP](https://drive.google.com/file/d/1_f1yP_agk3YOhwr4VLzQAunHAiWKag4J/view), SHA-256 `c7fe539acb1e865b65767532e2ccf56971cf79c94fbff8b0fa4456a04d8467da`, contains byte-for-byte matches for **311 of the original Git commit's 312 tracked files**. Only `LICENSE` differs, and no tracked file is missing. This includes the signed wallpaper image, its generated header and the source files discussed here. The [comparison report](docs/provenance/verification.json) ties the findings to the distributed release, rather than to changes made for this critique.

The original commit also tracks two Python bytecode files containing these source-path strings:

```text
tools/__pycache__/lzss.cpython-313.pyc:
  /mnt/data/pico_v21/PicoOS-Pro-v2.1/tools/lzss.py
tools/__pycache__/mkesp.cpython-313.pyc:
  /mnt/data/pico_v21/PicoOS-Pro-v2.1/tools/mkesp.py
```

Earlier archives contain paths under `/home/user/picoos/`; v2.0 contains `/home/user/myos/PicoOS-Pro-v2.0/tools/`. Together with the sandbox tooling and source-handoff language discussed below, these are concrete traces compatible with passing release archives through a hosted assistant environment. **That interpretation remains circumstantial:** directory names can be chosen by anyone, and a bytecode filename need not identify the machine or person that compiled it. They are not a vendor-specific code-generation record.

**The author's explanation for ZIP releases, followed by the GitHub update**

Asked in the separate r/AlternativeOS thread why the source was not on GitHub or Codeberg, the author [explained the workflow](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/comment/pdveaal/):

> I am currently using Google Drive just to quickly share the build images and the zip files while I focus 100% on the kernel development (like the new native USB drivers and the 3D graphics engine).
> However, moving the project to GitHub is definitely on my to-do list very soon! It will make it much easier for everyone to browse the source code and track the updates.

In [r/osdev, replying to a suggestion that Git would take less time](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe0e8id/), the author wrote:

> For me, dragging a new folder into Google Drive and making it public literally takes 1 or 2 minutes and I'm completely done. With Git, I would have to set up repositories and run a bunch of commands every time, which takes away time from writing code.

Those statements describe a preference for packaging and uploading complete directories. That fits the released archives, but does not identify who produced their contents. The v0.6 archive also contains an initialized `.git` directory with no object or ref files; the available evidence cannot turn a workflow preference into a finding about the author's programming ability.

The later updates matter too. In [r/AlternativeOS](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/comment/pe7dmse/), the author followed up:

> i did it on git!

And in [r/osdev](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/peeo87p/):

> no i use google drive for my iso's and github for the open source

The public GitHub import confirms that the source was subsequently published there. It would therefore be inaccurate to describe the current distribution as Drive-only. Publishing a snapshot still does not recover the missing development history.

**The desktop's CPU graph is a clock ratio, not a utilization measurement**

The author also promoted the live Task Manager in the [r/AlternativeOS announcement](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/):

> Custom Desktop Environment: A Windows/Ubuntu-style dark desktop interface with a Start-button launcher, application grid, live Task Manager (with process, memory, and background/foreground info), and a functional Calculator app.

In that desktop window, [the graph is explicitly labelled CPU](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L829-L847), but [its sampling function](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/menu.c#L807-L820) calculates:

```c
int cpu=(ms && dt)?(int)((dt*1000)/(ms*10)):0;
```

Here, `dt` is elapsed timer ticks and `ms` is elapsed milliseconds. The [API wrappers](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/exec.c#L43-L46) lead to [two readings of the same timer counter](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/timer.c#L3-L19), initialized at [100 Hz](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/main.c#L112-L113). For consistent samples without overflow, `ms = 10 * dt`, making the expression `1000 / 100 = 10`. Sampling races can introduce variation; actual CPU work is not an input.

This is a semantic failure: a graph can refresh and still measure the wrong thing. The kernel even has [separate accounting that excludes waiting tasks](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/task.c#L134-L142) and [exports per-task CPU ticks](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/task.c#L437-L453). The desktop calculation ignores that accounting.

**The standalone Task Manager refreshes the picture without refreshing its memory data**

In [the separate `taskmgr` application](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/taskmgr.c#L8-L12), `api->meminfo(&mi)` and `file_used()` run inside `if(first)`. The application then sets `first=0`. Its periodic update redraws memory and storage values and appends RAM-history samples using those retained values, without querying them again. Dragging the window sets `first=1` and causes another sample; ordinary timed refreshes do not.

The result is a graph labelled as live whose RAM samples remain stale between full redraws. This is specific to the standalone application: the integrated desktop Task Manager does re-query memory. It is evidence of a data-refresh defect, not evidence that every displayed statistic is invented.

**The defense invokes Quake, while the published port is DOOM**

The author invokes Quake in the defense quoted above and repeats the claim in [another reply](https://www.reddit.com/r/osdev/comments/1wxom7n/comment/pe0mvek/):

> If you think this project is just AI-generated, feel free to try and build a custom x86 kernel that boots and runs Quake yourself.

However, the author's own [r/AlternativeOS announcement](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/) identifies the port differently:

> DOOM Port: I successfully ported `doomgeneric` as `doom.pico`.

The actual [game entry point calls `doomgeneric_Create()` and `doomgeneric_Tick()`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/doom/pico/pico_main.c#L14-L24), and the [image build packages `doom.pico` and `doom1.wad`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/Makefile#L253-L265). The announcement agrees with the supplied implementation; the repeated Quake defense does not.

Searching this revision finds only two incidental Quake mentions, both inside imported DOOM sources: [an installation-directory comment](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/doom/src/d_iwad.c#L378) and [a header comment](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/apps/doom/src/z_zone.h#L15-L20). Neither is a Quake port. Invoking an implementation absent from the supplied evidence weakens the defense. A naming mistake or an unpublished build could explain this; the snapshot cannot establish that the author cannot distinguish the games.

**Further discrepancies in the published implementation**

The [r/AlternativeOS announcement](https://www.reddit.com/r/AlternativeOS/comments/1wxmcdn/picoos_pro_v21_my_custom_32bit_x86_os_written/) introduces its feature list and USB support as follows:

> Here is a quick overview of what is currently implemented and stable:
>
> Native USB Stack (NEW): Full native driver support for USB 1.1, 2.0, and 3.0 (UHCI/OHCI, EHCI, and xHCI controllers), handling mice and keyboards smoothly without relying on firmware emulation.

That makes stability a published claim about the release. The following discrepancies qualify that claim; they are not evidence that every controller or every tested machine fails:

- **The USB documentation contradicts the boot path.** The [original README says the native USB-HID driver was removed](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md#L48-L55). However, [`kmain()` calls `usb_init()`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/main.c#L200-L210), and [`usb_init()` probes xHCI, EHCI, OHCI and UHCI](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/usb.c#L446-L457). Native driver code is present and wired into startup. The README is stale or internally inconsistent.
- **The shell advertises networking that this tree stubs out.** Its [version text lists Ethernet, TCP, DHCP, DNS and HTTP support](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/shell.c#L960-L968), but the reviewed tree has no `net/` directory. The [Makefile selects the fallback](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/Makefile#L16-L22), whose functions [report zero interfaces and fail network operations](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/stubs/net_stub.c#L12-L45).
- **OHCI endpoint linking contains an initialization-order error.** [`ohci_init_one()` zeroes the controller structure](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/ohci.c#L269-L272). Its [allocation loop assigns each descriptor's `next` from the following slot before that slot is allocated](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/ohci.c#L295-L302). Consequently, all four `next` values are zero, and every interrupt-table entry points to the first descriptor. This contradicts the file's [description of a chain covering every interrupt endpoint](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/ohci.c#L3-L11). This finding comes from static inspection; no hardware failure is claimed to have been reproduced.

**Why these bugs matter to the authorship argument**

The CPU and RAM defects occur in routine display updates, without requiring unusual hardware or an obscure input. They illustrate a gap between making a feature look complete and checking what it actually measures. In the CPU case, suitable accounting already exists in the kernel, yet the desktop uses an unrelated clock ratio. In the RAM case, the application redraws its data without resampling it. These are concrete integration and validation failures.

An explanation involving generated components assembled without sufficient review is compatible with that pattern. So are mistakes in manually written code. Calling these bugs impossible for a human would turn a checkable technical criticism into an unsupported authorship claim. Their evidential value is that the implementation fails to support the feature descriptions; they cannot identify who produced it.

**A successful build cannot establish human-only authorship**

The repository's [Makefile selects GCC and invokes it to compile kernel sources](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/Makefile#L1-L61). GCC can compile code whether it was written by a person, generated by a model, or edited by both. A working executable therefore cannot resolve that distinction.

AI coding tools can also operate compilers and test harnesses. Anthropic's [24 February 2025 introduction of Claude Code](https://www.anthropic.com/news/claude-3-7-sonnet) documents its ability to edit files, execute tests and use command-line tools. Its [5 February 2026 compiler experiment](https://www.anthropic.com/engineering/building-c-compiler) reports an AI-written C compiler capable of building a bootable Linux kernel and compiling and running DOOM, with [public source code](https://github.com/anthropics/claudes-c-compiler). The report also describes substantial human work on the harness and limitations, including reliance on GCC for 16-bit x86 boot code.

These sources rebut the general premise that working low-level software excludes AI involvement. They do not demonstrate the exact Quake port described in the reply, or connect any particular AI tool to PicoOS. The port documented in this snapshot is [DOOM via doomgeneric](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/docs/DOOM.md#L1-L6).

**The circumstantial case for AI assistance: request-oriented comments**

The USB implementation contains this sentence:

> Why the keyboard too, when only the mouse was asked for:

It then explains why taking over the USB controller requires keyboard support as well. This is an explicit reference to the scope of a request, embedded in the source itself. [Source: `kernel/usb.c`, lines 19–23.](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/kernel/usb.c#L19-L23)

**Interpretation:** this could be an assistant explaining an implementation that exceeded a prompt's literal scope. It could equally be a human documenting a feature request. The comment supports investigating how the work was requested; it does not identify the implementer.

**The tooling describes a restricted, temporary sandbox**

The environment setup script states:

> This sandbox has no root and wipes /tmp between sessions

It downloads Debian packages and extracts them into a temporary prefix. This is implemented behavior, not merely a suggestive comment. [Source: `tools/devenv.sh`, lines 1–49.](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/devenv.sh#L1-L49)

Two other files corroborate that environment: the [QEMU automation defaults to binaries and libraries under `/tmp/qroot`](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/screen.py#L18-L20), and a [pure-Python screenshot converter explains that the sandbox lacks image tools and PIL](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/tools/ppm2png.py#L1-L8).

**Interpretation:** these are consistent with development inside a hosted coding sandbox, including one operated by an AI assistant. Human-operated containers, restricted accounts and remote development environments can have the same constraints. These files do not establish a vendor, model or AI involvement by themselves.

**The documentation retains source-handoff language**

The original README describes starting from supplied PicoOS 1.1 sources and later says:

> The old source tree at `/home/user/picoos-old/picoos`
> is intentionally not modified.

That records a workspace-specific preservation decision. [Sources: original README, lines 29–32](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md#L29-L32) and [125–126](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/README.md#L125-L126).

Similarly, the [network stub's introduction](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/stubs/net_stub.c#L1-L6) explains that a source ZIP lacked the network directory and tells its recipient how to restore it.

**Interpretation:** this resembles a handoff report from someone modifying a provided archive, which is compatible with an assistant workflow. It is also compatible with a human collaborator or the author moving between their own archives. These passages cannot establish undisclosed AI use, and should not be counted as independent proof merely because they appear in several files.

**What remains unproven**

The inspected revision has [one initial import commit](https://github.com/PicoOS-Pro-v/picoOS/commit/c8142cef3cf7bc28906c947fab53d58385ef823a), so its Git history cannot reconstruct the development process. The release archives provide snapshots, but no attributable conversation or generation record for the kernel was found in this review. The wallpaper has direct AI provenance; extending that finding to all source code would require further evidence.

The bundled DOOM sources also should not be presented as tens of thousands of lines newly written by this author or by an AI: the project [explicitly credits doomgeneric and distinguishes its platform layer](https://github.com/PicoOS-Pro-v/picoOS/blob/c8142cef3cf7bc28906c947fab53d58385ef823a/docs/DOOM.md#L50-L65).

The authenticated image establishes AI-generated content in the release. The measurable defects and contradictory descriptions establish specific failures of implementation and validation. Substantial AI coding assistance is compatible with the request, sandbox and handoff artifacts, but the available records do not establish the division of programming work. A running game cannot prove human-only authorship; these bugs cannot prove total ignorance either.

To reproduce the provenance checks, create an isolated Python environment and run the [verification script](tools/verify_provenance.py). It reads the fixed original Git revision, downloads hash-pinned official trust lists, verifies the C2PA record, regenerates the wallpaper into a temporary directory and reports the bytecode strings. Passing a downloaded v2.1 ZIP also repeats the 312-file comparison.

```sh
python3 -m venv /tmp/slopos-provenance-check
/tmp/slopos-provenance-check/bin/pip install c2pa-python==0.38.0 Pillow==12.3.0 numpy==2.5.3
/tmp/slopos-provenance-check/bin/python tools/verify_provenance.py
# Optional: append the path to the public release ZIP:
/tmp/slopos-provenance-check/bin/python tools/verify_provenance.py /path/to/PicoOS-Pro-v2.1.zip
```

To inspect the same evidence locally, use the fixed revision rather than the changing branch head:

```sh
review_commit=c8142cef3cf7bc28906c947fab53d58385ef823a
git rev-list --count "$review_commit"
git show "$review_commit:kernel/usb.c" | sed -n '15,24p'
git show "$review_commit:tools/devenv.sh" | sed -n '1,49p'
git show "$review_commit:kernel/ohci.c" | sed -n '269,302p'
git show "$review_commit:apps/menu.c" | sed -n '807,847p'
git show "$review_commit:kernel/timer.c" | sed -n '1,19p'
git show "$review_commit:apps/taskmgr.c" | sed -n '8,12p'
git grep -n -i quake "$review_commit" -- . ':!doom1.wad'
git ls-tree -r --name-only "$review_commit" -- net/
```

This replacement README and its verification script were prepared with AI assistance. The review covered the public discussion, repository history, twelve release archives, signed image metadata, selected kernel and application code, build tooling and documentation. It was not an exhaustive audit or a runtime validation of the OS.
