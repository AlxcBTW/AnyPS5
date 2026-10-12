# Sword of the Sea working group (lab build)

Branch `sots/next` on https://github.com/AlxcBTW/AnyPS5 is a tree-only snapshot of our lab base for
Sword of the Sea (PPSA25344, Unreal Engine 5). It is for testing with us, not a proposal to upstream.
AnyPS5 is a PS5 compatibility layer / relinker (like Wine), not an emulator.

## What works

New Game -> intro cutscene, no device loss, about 1.5 FPS. Not playable yet.

## Prerequisites

- Your own dump from your own PS5. The game is in the PS Plus Extra catalog.
- NVIDIA or AMD GPU with a Vulkan 1.1+ driver.
- Windows 11 or Linux, x86-64. (Windows needs MinGW-w64 GCC 15.2.0, see BUILD.md.)

## Build

```sh
git clone -b sots/next https://github.com/AlxcBTW/AnyPS5.git
cd AnyPS5
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel
cmake --build build --target libs --parallel
```

Requirements and Windows/Linux specifics: [docs/dev/BUILD.md](docs/dev/BUILD.md). The `libs` target
is not part of the default build; rerun it after every library change, or you test old binaries.

## Relink and run

Details: [docs/user/USAGE.md](docs/user/USAGE.md). Summary, Windows:

```sh
build/core/relinker/relinker --windows source/eboot.elf app.exe
```

Omit `--windows` for Linux. Put the game's `sce_module/` (or `sce_modules/`, `prx/`) beside the input
ELF, the game files in `app0/`, and copy `build/core/libs/libs/*.prx` into `libs/` beside the output.
Never put the original PS5 `.prx` files into `libs/`. Then start `./app.elf` or `.\app.exe`.

## Recommended environment

```sh
APS5_RESOURCE_CACHE_ENTRIES=8192   # default is 1024
APS5_PROFILE_DRAW=1                # profile lines on stderr
APS5_HOST_IMPORT_JOIN=1            # join host imports of adjacent large ranges
APS5_LOCAL_PAD_SCRIPT="45:cross:1000,75:cross:1000"   # presses New Game at 45 s and 75 s, 1000 ms each
```

Also pass the Unreal argument `-nothreadtimeout` to the game (`./app.elf -nothreadtimeout`;
upstream docs do not cover game arguments, so this is our lab practice).
`APS5_LOCAL_PAD_SCRIPT` is a lab aid (`<seconds>:<button>[:<hold ms>],...`). The any-size join of this
snapshot can be switched off with `APS5_EXP_JOIN_EXACT=0` if you suspect it.

## What to report

- GPU model, driver version, OS.
- The FPS shown in the window title.
- Screenshots at fixed times: 30 s, 60 s, 120 s, and at the first scene after New Game.
- Every `[gpu]` line and every `device lost` line from stderr (attach the log, not the dump).
- The commit sha (`git rev-parse --short HEAD`) of the build you tested.

## Ground rules

- Never share game data, dumps, shader caches or run directories. Not in Discord, issues or PRs.
- One game at a time: Sword of the Sea only on this branch.
- Report regressions with the commit sha and what changed versus your last good run.

## Known issues

- Black area at the first scene.
- The sword has no glow.
- Orange screen at the start of gameplay.
- Low FPS (about 1.5).

## Contact

Ping us on Discord.

## Included changes

Base: upstream `main` at a11c8f7f7 (merge of #3511) plus the commits below, in log order. Authorship is
preserved. The snapshot also contains our lab diagnostics and experiments (`exp:` / `local:` commits);
they are env-gated and off by default, except where named above. PR numbers appear only where the
commit subject carries one.

- Celegans12: feat(agc): implement image and sampler tables indexed at runtime offsets
- Celegans12: feat(agc): implement image tables loaded through 64-bit pointers
- Scramblrrrr: fix(shader): preserve distinct definitions while folding phi inputs
- Scramblrrrr: fix(shader): keep deferred specialization rewrites valid
- Scramblrrrr: chore: integrate canonical cube output modifiers
- Scramblrrrr: chore: integrate canonical asynchronous APR on current runtime
- Scramblrrrr: fix(shader): fold immediate descriptor bit reversal
- Scramblrrrr: fix(libkernel): reuse checked close for POSIX descriptors
- Michon: feat(agc): depth surfaces read as textures and storage images
- Michon: fix(agc): storage access to a depth surface's stencil plane keeps throwing; seeding applies a pending fast clear
- Michon: style(agc): drop code comments the conventions check rejects
- Michon: fix(agc): depth surface reads on top of main's reused-memory lookups and #374
- SP4C3B4R-8: fix(libSceAgcDriver): retire a depth surface once another writer covers its memory
- SP4C3B4R-8: fix(libSceAgcDriver): binding a depth target retires the depth surfaces it overlaps
- SP4C3B4R-8: fix(libSceAgcDriver): a written storage image the depth plane cannot seed retires the depth surface
- AlxcBTW: fix(recompiler): merge a loop selection where its arms meet in the iteration
- AlxcBTW: fix(recompiler): route loop exits that leave the loop before they meet
- AlxcBTW: fix(recompiler): reject a loop-exit pass result that is not structured
- Scramblrrrr: fix(recompiler): route shared loop exit selections in the guarded fallback
- Scramblrrrr: fix(recompiler): privatize shared continuations before loop gateways
- Scramblrrrr: fix(recompiler): preserve the guarded loop exit fallback before shared routing
- Scramblrrrr: test(shader): expect main to merge the shared exit tail without routing
- Scramblrrrr: perf(shader): avoid rerouting synthetic loop-exit forwarders
- Scramblrrrr: fix(shader): validate conditional exits before accepting every fallback
- Scramblrrrr: refactor(shader): drop the overlapping global loop join fallback
- AlxcBTW: fix(agc): treat other-shaped storage views of a depth surface's memory as reused memory
- AlxcBTW: test(agc): cover storage views of a depth surface's memory
- Scramblrrrr: test(agc): match depth seeding refusal in the cross-test stack
- jdmagic: feat(agc): storage images over a depth surface's stencil plane
- esatturan: fix(libkernel): map direct memory as write-watched private pages until it is aliased
- AlxcBTW: test(libkernel): alias the direct memory of the failed-collect check
- AlxcBTW: feat(shader): APS5_TRACE_ANCILLARY names why a select keeps the ancillary word
- AlxcBTW: feat(shader): give reads the masked-select pass keeps the whole pixel ancillary word
- Michon: feat(agc): vertex clip and cull distances (#423 at 577b8d57, squashed)
- john.maia: fix(agc): prepare vertex programs with the PA_CL_VS_OUT_CNTL position export layout
- alfalfa-star: feat(agc): render layered draws into 3D color targets
- alfalfa-star: test(agc): read #423's legacy vertex info as version 16
- alfalfa-star: test(agc): pass the GPU flag to Mutation::Add in the layered volume test
- alfalfa-star: test(agc): draw a layered volume through the driver
- alfalfa-star: fix(agc): report layer exports as ignored only when a draw does not route them
- alfalfa-star: docs(agc): record the DCC key extent assumed for 3D color targets
- AlxcBTW: fix(agc): layered draws through dynamic rendering; port the layered tests
- AlxcBTW: test(agc): end the clip and cull draw test with the libc shutdown
- AlxcBTW: fix(recompiler): give wave64 vertex programs on 32-lane hosts a 32-lane s3
- AlxcBTW: test(agc): run a wave64 NGG full-wave OR on a 32-lane device
- AlxcBTW: fix(recompiler): keep lane 31 a shuffle without subgroup arithmetic
- AlxcBTW: feat(input): turn mouse speed into a smooth right-stick tilt
- AlxcBTW: feat(input): give keyboard sticks partial tilt and an optional ramp
- AlxcBTW: feat(input): read per-game input profiles by title ID
- AlxcBTW: test(input): simulate keyboard and mouse sticks offline
- AlxcBTW: feat(tools): add a pad visualiser for tuning keyboard and mouse sticks
- AlxcBTW: feat(input): bind L2, the mouse side buttons and a left-hand face row
- AlxcBTW: feat(input): list the bindings at start-up and show mouse capture in the title
- AlxcBTW: feat(input): toggle mouse capture with F7 and release it on focus loss
- AlxcBTW: feat(input): let keys ramp the triggers and the wheel hold them
- AlxcBTW: feat(input): choose whether keys reach the pad, the game, or both
- AlxcBTW: feat(input): let the captured mouse drive the left stick
- AlxcBTW: fix(input): read input files from paths with non-ASCII characters
- AlxcBTW: feat(input): smooth slow-reporting mice by their measured report rate
- AlxcBTW: perf(shader): drop the host length branch on bound buffers under robustBufferAccess2
- Karan Vora: perf(shader): pick f16 min, max and med3 results on the bits
- AlxcBTW: perf(shader): convert packed f16 results without the NaN fix-up where the lanes are rewritten
- AlxcBTW: perf(agc): create compute pipelines outside the GPU mutex
- DotDebian: perf(libc): size the GetWriteWatch buffer to the collected range
- DotDebian: perf(agc): look ahead of the queue workers' write-watch walks on Windows
- DotDebian: style(agc): drop the comments of the write-watch prewalk
- AlxcBTW: perf(libc): resolve watched private allocations from the map before VirtualQuery
- AlxcBTW: perf(libc): resolve collected private pieces only through the allocation map
- AlxcBTW: Revert "fix(shader): fold immediate descriptor bit reversal"
- AlxcBTW: bisect: map direct memory shared, never as write-watched private pages (undo #2054 effect)
- AlxcBTW: bisect: drop the Windows-only call from the non-Windows branch
- alfalfa-star: fix(agc): take an HTILE fill from a value in memory as a depth fast clear
- AlxcBTW: fix(shader): apply VOP3P neg_lo/neg_hi to packed 16-bit integer sources
- AlxcBTW: fix(libc): alias shared guest memory across several sections or non-contiguous offsets
- AlxcBTW: test(agc): a host import dropped mid-hold must outlive the hold's later batches
- AlxcBTW: fix(agc): keep a host import dropped mid-hold for every batch that hold opens
- Sanjaysrinivas: fix(agc): reclaim idle buffers on allocation OOM
- Felipe Arce: fix(agc): keep another device's host imports until its teardown
- Michon: feat(agc): host import windows, least-recent eviction under the budget
- Michon: test(agc): host import windows test on main's Mutation::Add gpu flag
- Michon: fix(agc): an address-based build imports the whole range, not its first window
- Chad: perf(libc): walk shared views by iterator and remember clean pages per run in the write collect
