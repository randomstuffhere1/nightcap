# Performance and CPU load

What Nightcap does to your CPU and GPU, which knobs change it, and how to
measure rather than guess.

## GameMode core pinning

Nightcap asks Feral GameMode for a performance session at startup
(`performance.gamemode`, default `auto`). GameMode raises the CPU governor and
the process priority, which helps.

On a CPU with a wide spread of per-core maximum frequencies — Intel P-core and
E-core parts, AMD parts with preferred cores — GameMode also pins the process
to the fastest cores. On a 32-thread host that can mean 4 usable threads out of
32. Roblox spreads its work across a worker pool, so the pin concentrates that
pool onto a couple of physical cores instead of spreading it. Those cores sit
near their boost ceiling and run hot while the rest of the chip idles.

GameMode re-applies the pin roughly every 5 seconds, so widening the mask from
inside the process does not hold. Releasing the request does release the pin.

Under `auto`, Nightcap therefore checks its CPU affinity immediately after the
request. If GameMode narrowed it, Nightcap ends the request and prints:

```
  [gamemode] released: it pinned this process to a subset of the CPUs, ...
```

The trade is that GameMode's governor and priority boost go with it. To keep
those and lose only the pinning, turn pinning off for every game on the host.

These are the settings to run with:

```ini
# ~/.config/gamemode.ini
[general]
desiredgov=performance

[cpu]
pin_cores=no
park_cores=no
```

`desiredgov` is the one to check first. The packaged default in
`/usr/share/gamemode/gamemode.ini` is already `performance`, but a value in
your own file replaces it, and `desiredgov=powersave` there means GameMode
clocks the CPU **down** for the whole session while still reporting
`[gamemode] performance request active`. The symptoms are a low CPU clock
under load, a GPU that never leaves its lower SM clocks, and the engine pump
falling back to `tpause` — which costs about 70% of a core instead of 3%,
because `sleep` is only chosen when the governor reads `performance`. On one
13900K host, correcting `powersave` to `performance` took the process from
177% CPU to 42% and the package from 70°C to 53-63°C, at the same frame rate.

GameMode reads `$XDG_CONFIG_HOME`, `/usr/share/gamemode` and `/etc`, in that
order. It does not read anything under Nightcap's own config directory, so this
file has to live in one of those three places. The daemon reads it when it
starts, so restart it after editing: `systemctl --user restart gamemoded`.

Restarting the daemon drops the session of any game already running; relaunch
the game for new settings to apply to it.

`gamemoded` is D-Bus activated through
`com.feralinteractive.GameMode.service`, so `systemctl --user stop gamemoded`
does not hold — the next client request starts it again within seconds. To
measure with GameMode genuinely off, mask it:

```bash
systemctl --user mask gamemoded     # and --unmask afterwards
```

The governor switch runs through polkit. Debian and Ubuntu only allow it for
members of the `gamemode` group; without that, the daemon's journal shows
`cpugovctl set performance: Not authorized` and the governor stays put while
the session is otherwise active. Add yourself and log in again:

```bash
sudo usermod -aG gamemode "$USER"
```

Fedora and Arch allow any logged-in local user.

Setting `performance.gamemode: on` accepts the pinning instead: `on` is an
explicit request for GameMode, and Nightcap keeps the session whatever it does
to the affinity. `off` never contacts the daemon.

To see which CPUs the process may use:

```bash
pid=$(pgrep -x Main)
for t in /proc/$pid/task/*; do taskset -pc "${t##*/}"; done
```

Two traps here. The engine process reports its name as `Main`, not
`mocktail`; `pgrep -x mocktail` finds only the launcher, which sits near 0%
CPU and tells you nothing. And `taskset -p` reads one thread, not the whole
process, so a single call reports the main thread's mask and says nothing
about the workers — which is where the game's load actually is.

For the same reason, `ps -L -o psr=` shows which core each thread is running
on right now, not which cores it is allowed on. On a hybrid CPU the scheduler
preferring P-cores for runnable work is normal and is not a mask.

## The main-thread pump

Roblox posts its per-frame main-thread step at a time of its own choosing,
and nothing signals the host when it does. The host main thread therefore
polls `nativeCallMessagesFromMainThread`, and a post that waits more than a
fraction of a millisecond costs a whole frame. Between polls the thread rests
for 100 µs, and how it rests decides the CPU clock:

| Mode | When | What it costs |
| --- | --- | --- |
| `sleep` | The governor is `performance`, which GameMode sets | The thread idles at about 3% of a core. The governor holds the clock. |
| `tpause` | Otherwise, on a CPU with WAITPKG (Intel 12th gen and newer) | The core stays awake in a light C0 state. Frame rate holds; the OS books the time as busy, so the thread shows around 70% of a core, at lower power than a spin. |
| `spin` | Otherwise | The core stays awake at full power. |

Under a power-saving governor the CPU clocks a bursty core down unless some
core stays awake, and Roblox's render thread then runs slower and drops
frames. That is why `sleep` is not the default there. Startup prints the
chosen mode:

```
  [main] engine pump rest: tpause (MOCKTAIL_ENGINE_PUMP_REST=sleep|tpause|spin)
```

`MOCKTAIL_ENGINE_PUMP_REST` forces a mode. `sleep` without the governor's help
runs cooler for a few frames per second less; measure before keeping it.

## When the engine stops rendering

A trace sometimes shows freezes with nothing in them: the main thread calls the
engine, the render thread goes quiet, and no frames come out. The obvious
reading is that something is blocked. **It is not.** This section records what
was measured, because the obvious reading is wrong and it is easy to waste time
on it.

A `--profile` trace carries `cpu_us` on every `nativeCallMessagesFromMainThread`
slice, so you can see this without any extra tooling. Compare the *cadence*, not
just the CPU:

| | inside a 1.4 s freeze | healthy stretch |
| --- | --- | --- |
| pump interval, median | 4.17 ms | 4.17 ms |
| main thread busy | 7.5% | 9.0% |
| render thread busy | **0.2%** | 3.2% |

The main thread is calling the engine at exactly the normal rate and returning
immediately. The render thread has nothing to do. The engine is polling, finding
no work, and choosing not to render — which is a different problem from blocking,
and one that is usually not on this side of the bridge.

`cpu_us` far below `dur` on its own does **not** mean blocked. An idle thread
that polls on a timer looks identical to a blocked one. The cadence is what
tells them apart: a blocked thread's pump interval stretches, an idle one's does
not. A long `dur` with a low `cpu_us` at startup is usually the engine's own
initialisation phase, not a stall.

`strace` settles what the host is doing. Across a 141 s session containing
multi-second freezes, the blocking-call totals were:

| syscall | total blocked | count |
| --- | --- | --- |
| futex | 6,655 ms | 2,139 |
| poll | 2,729 ms | 752 |
| ppoll | 751 ms | 83 |
| epoll_wait | 386 ms | 250 |
| `recvfrom`/`recvmsg` | 141 ms | 3 |

Three network reads over 50 ms in the whole session, all at startup. Nothing
waits for anything close to the freeze durations, and there were 21,616
`EAGAIN` returns from non-blocking reads — the guest polling descriptors that
have nothing on them, which is the signature of polling rather than waiting.

Beware what strace does to the numbers. It adds overhead per syscall, and the
session above had its worst frame stretched to 28.9 s under it, against 1.4 s
untraced. Use it to rule causes out, not to measure.

### The freeze pattern

Gap durations cluster near exact second multiples, which reads like a retry or a
poll interval:

```
999.3  999.6  999.9  999.9  1062.6  930.5     mean 998.7 ms
```

One tempting explanation is a DNS or asset-resolution storm: a trace under
strace showed `inet_addr` called 12,408 times for a single address, 808 times in
one second, during the largest gap. Checked against the gaps rather than the
seconds, it does not hold. Fourteen of the ~1 s gaps contain essentially zero
such calls, and the correlation over the whole session is r = −0.07. It explains
one large gap and nothing else. If you chase this, check per-gap coverage
before believing a per-second correlation.

**No host-side cause has been found.** Every layer instrumented so far says the
engine is idling, not blocked, and the most likely explanation is engine or
server-side behavior reached through the guest. If you are looking for
something to fix here, this section is the honest answer: there may not be one.

### If you still want the stack

For a session where a *single* pump call really is long, this names the frames
underneath it:

```sh
MOCKTAIL_TRACE_PUMP_STALLS=1 mocktail
```

```
  [stall] engine main-thread step blocked 347 ms with no CPU, driven from main
        #0 libroblox.so+0x4a1b2c
        #1 libc.so.6+0x111 [read]
```

It reports per-call duration, which is the wrong axis for the freeze pattern
above — the pump calls during a freeze are *shorter* than normal. It is still
the right tool for a genuinely long individual call.

A Release build carries no symbols for the guest, so each frame is named by the
image it falls in, read from `/proc/self/maps`, with a real symbol appended when
one exists. That separates a frame in `libroblox.so` from one in a system
library's `read`, but it will not name a function inside the guest; build with
`-DCMAKE_BUILD_TYPE=Debug` when the function matters.

Two more limits:

- A call driven as a thread-join poll callback is not reported. The step is idle
  by construction there, so a long call while joining the startup thread is the
  join working.
- If the unwind carries nothing below the pump, the log says so rather than
  implying a cause was found:
  `blocked in host code; no frame below the pump`.

Only the top frames identify a wait, so repeats are counted as one shape and the
stack is printed once rather than every time. A new shape is always printed,
even inside another shape's window.

Two knobs, both optional:

- `MOCKTAIL_PUMP_STALL_MIN_MS` (default 50) is the threshold at which a step
  counts. The pump runs at kilohertz rates, so this matters more than the toggle.
- `MOCKTAIL_PUMP_STALL_COOLDOWN_MS` (default 1000) is the shortest gap between
  two captures. Capturing a stack costs a symbol lookup, and a session that never
  stalls should not pay for a diagnostic nobody reads.

Off unless one of these is set. `MOCKTAIL_TRACE_MAIN_THREAD_PUMP` turns it on
as well, since that already means someone is diagnosing this.

## Settings that ask for more work

Three settings forward a level or a budget rather than changing how Nightcap
behaves. None of them is a measured speedup, and the honest reason each exists
is that the value the client or the launch policy would pick is not always the
one you want.

`graphics.quality` (`auto` by default, or 1 to 6) forwards Roblox's
`FIntDebugFRMQualityLevelOverride`. `auto` leaves the level to the client. A
number overrides it, which matters in two places: the multithreaded rendering
path asks for a level of its own, and `graphics_launch_policy.cc` drops to
level 1 on an Intel-only machine. Both use a default, so an explicit value in
`config.yaml` wins over both.

A higher level is not better output. It asks for more shadow and lighting work.
On hardware that cannot afford it, that is a lower frame rate, not a sharper
image. Lowering it is the direction that helps a struggling machine.

`performance.texture_memory_mb` (0 by default) overrides the texture budget
Roblox is told about. 0 keeps the automatic figure, which is an eighth of host
RAM clamped to `[256, 1536]` MiB. An explicit value is used as given rather than
clamped back to that range, because asking for more than the rule allows is the
whole point of asking.

**The ceiling is 2047 MiB, not 65536.** The client reads this through
`FIntRenderForceVideoMemorySize`, a signed 32-bit setting, so a budget past
`INT32_MAX` bytes cannot be represented and is refused rather than truncated.
That is barely above the automatic 1536 MiB ceiling, so this setting is a way to
*lower* the budget or to nudge it up by a few hundred MiB, not a way to give a
large GPU many gigabytes. The resolved budget is printed at startup:

```
  [perf] texture memory budget: 1024 MiB (from host RAM)
  [perf] texture memory budget: 4096 MiB (configured)
```

`performance.small_texture_upscale` (4 by default) is described under
[Small texture upscaling](#small-texture-upscaling) below. 1 turns it off.

`audio.buffer_samples` (0 by default) is advisory and mostly diagnostic. SDL3
exposes no way to set a device buffer size, so the value cannot be pushed to
the driver from here; `SDL_AUDIO_BUFFER_SAMPLES` is the supported way to
influence it. What the setting does is make Nightcap say so at startup rather
than leave you guessing:

```
  [audio] buffer: 1024 frames (audio.buffer_samples 512 requested; SDL3 sets this per device, so raise SDL_AUDIO_BUFFER_SAMPLES to change it)
```

The useful half is the starvation counter. SDL has no underrun callback, so
starvation is counted where it is visible: a device refill that ended with less
than the engine owed. The count is reported once at shutdown, and a large one
means the buffer really is too small.

```
  [audio] starved refills: 2487 (audio.buffer_samples is likely too small)
```

That counter is worth having whatever you set. A device that starves
constantly is a latency problem that no frame-time number will explain.

## Small texture upscaling

`performance.small_texture_upscale` in `config.yaml` sets this;
`MOCKTAIL_SMALL_TEXTURE_UPSCALE` does the same from the environment. The default
is 4, which redraws ETC2 textures of 64px or smaller at N times their width and
height. At the default that is 16 times the pixels per affected texture.

The cost lands in two places. Decoding and resampling is CPU work on up to 8
threads while textures load, which shows up as a burst rather than a steady
load. The enlarged textures then stay in GPU memory for the session and cost
extra sampling bandwidth on every frame that uses them.

That decode does not run on the thread that submits the frame. On a device
with a timeline semaphore, uploads decode on a background dispatcher and the
submit carries a semaphore wait, so the GPU waits for the texture instead of
the render thread. In the trace this is the `etc2 decode` slice on its own
thread, with only a negligible `etc2 gather` left on the submitting one. A
device without that feature decodes inline as before.

Set it to `1` to turn upscaling off, in `config.yaml` or the environment:

```bash
MOCKTAIL_SMALL_TEXTURE_UPSCALE=1 mocktail
```

Only experiences that upload compressed textures are affected. A trace of a
session in a place that used none recorded no `etc2 decode` slices at all, so
on such a place this setting cannot matter in either direction.

### Which resampler runs, and what it is worth

`ResampleRgba` -- the box filter that widens textures -- has two implementations:
a C++ one in `src/graphics/texture_override.cc` and a Rust one in
`rust/crates/mocktail-rs-kernels`. They are bit-for-bit identical, and
`kernel_parity_test` proves it by running both over every mode, stride and
dimension combination and comparing the destinations byte for byte.

`MOCKTAIL_RUST_KERNELS=off` selects the C++ one; anything else, including unset,
runs the Rust one. The first upload of a session prints which is live and what
SIMD class the host has:

```
  [vulkan] resample: rust kernel, simd 2 (MOCKTAIL_RUST_KERNELS=off selects the C++ kernel)
```

`simd` is 0 portable, 1 SSE2, 2 SSSE3, 3 AVX2, 4 NEON. Detection is CPUID rather
than `std::is_x86_feature_detected!`, which would pull `std_detect` into every
image that links the kernel. It is read once and cached in an atomic: CPUID is a
serialising instruction costing around a hundred cycles, and the first version
called it per block, which made the EAC alpha merge *four times slower* than the
C++ it was meant to beat -- 329 us against 1529 us for 16384 blocks, measured.

The AVX2 level needs `XGETBV`, which is not in the x86-64 baseline and lowers to
a libgcc helper when called from ordinary code, dragging about 400 KB of `std`
backtrace machinery into every image that links a kernel. Wrapping the query in
a `#[target_feature(enable = "xsave")]` function inlines it as the instruction
it is and keeps that out of the link: `libvulkan.so` grew by 264 bytes.

Measured on one 4-core machine, `taskset -c 2 build/tests/kernel_benchmark`, best
of nine trials of a hundred iterations, against the C++ path forced off:

| case | C++ | Rust | |
| --- | --- | --- | --- |
| 64x64 -> 64x64 | 28.5 us | 15.3 us | **1.89x** |
| 64x64 -> 128x128 (x2) | 60.9 us | 23.8 us | **2.56x** |
| 64x64 -> 256x256 (x4) | 133.4 us | 34.2 us | **3.91x** |
| 32x32 -> 128x128 (x4) | 33.6 us | 9.7 us | **3.47x** |
| 128x128 -> 256x256 (x2) | 251.0 us | 90.3 us | **2.78x** |
| 256x256 -> 1024x1024 (x4) | 2199 us | 632 us | **3.48x** |

The integer upscales are the ones that matter, because that is what
`small_texture_upscale` produces. They are the fastest cases, not the slowest:
the Rust path recognises an integer replicate and writes runs of texels with
whole registers, where the C++ read a column table per destination pixel.

The largest case gained least of all earlier -- 1.21x -- because at 1024x1024 it
writes 4 MiB per call and takes 13.6 million cache misses, which is memory
bandwidth rather than arithmetic. It now reads 3.48x anyway, so the previous
figure was measuring something other than the ceiling: that column was timing
the C++ path twice. See the benchmarking note below.

### The ETC2/EAC kernels

`DecodeEtcImageBlockRows` has its EAC alpha merge and its whole R11/RG11 family
in Rust (`MOCKTAIL_RUST_ETC=off` selects the C++), also checked bit-for-bit by
`kernel_parity_test`:

| case | C++ | Rust | |
| --- | --- | --- | --- |
| ETC2_RGB8 128x128 | 386 us | 380 us | 1.01x |
| ETC2_RGBA8 128x128 | 592 us | 497 us | **1.19x** |
| ETC2_RGBA8 64x64 | 142 us | 118 us | **1.20x** |
| EAC_R11 64x64 | 96 us | 37 us | **2.56x** |
| EAC_RG11 64x64 | 169 us | 69 us | **2.45x** |
| EAC_RG11 256x256 | 2758 us | 1188 us | **2.32x** |

`ETC2_RGB8` is the noise floor and the reason the rest can be believed: it never
calls into Rust at all, so 1.01x is what a null result looks like here.
`DecodeRgbBlock` stays in C++ -- its SSSE3 palette shuffle is already as good as
anything -- and it is what holds the RGB formats near flat.

The R11 family is where the width paid, and the first version of it was *slower
than the C++* despite being bit-identical. Three things turned that around, in
descending order of how much they were worth:

* **Raw pointers instead of slices.** `block[i]` in the six-byte index read was
  six bounds checks per sub-block, where the C++ pointer arithmetic has none.
  Worth roughly 1.9x on its own.
* **The 16 indices extracted once per sub-block.** The C++ recomputes
  `bits >> (45 - texel * 3)` per texel *per channel*, so an RG11 block shifts the
  same 48-bit field thirty-two times for sixteen texels.
* **Branchless sign handling in the signed formats.** `value < 0 ? -value : value`
  followed by a second test to negate is two data-dependent branches per palette
  entry, and eight entries of mixed sign mispredict often enough to cost more
  than the arithmetic. An arithmetic shift gives an all-ones mask; two's
  complement negation is xor-then-add-**one**, since adding the mask itself
  negates to `-x - 1`, which the parity suite caught.

The alpha palette is eight independent `clamp(base + table[i] * mult, 0, 255)`
computations, which is what AVX2 is for: one 32-byte table load, a multiply, an
add, a clamp pair and a pack, replacing thirty-odd scalar ops. The table is
stored flat rather than `[[i32; 8]; 16]` so that one row *is* one register.

In isolation, against transcribed copies of the C++ kernels, the ported kernels
measure **1.72x** (alpha merge), **2.14x** (R11), **2.15x** (RG11) and **2.26x**
(RG11 signed). Whole-image ratios are lower than these because the C++
`DecodeRgbBlock` and the edge-clamped writes are shared cost that neither side
can improve on.

The decode pool moved to rayon and reads 1.00x across every batched and
concurrent case. The C++ `DecodePool` declines when it is busy and lets the
caller drain the batch itself, which looks bad on paper, but with N caller
threads on N cores the machine is saturated either way. The change is kept
because it removes a deliberate thread leak and the fixed 4096-block banding,
not because it is faster. `MOCKTAIL_RUST_POOL=off` puts the C++ pool back.

### Measuring these numbers, and one way they were wrong

The A/B switches latch the environment on first use, because they are consulted
per block and `getenv` is a linear scan. A `setenv` after that first call is
therefore ignored -- and the benchmark that times both variants in one process
was setting the variable to `off` for the C++ column and `on` for the Rust one.
Whichever column ran first won, both columns ran the C++ path, and every ETC2
ratio printed a confident 1.00x against itself. The resampler escaped it only
because that column called the FFI directly.

`SetRustEtcToggle` and its two siblings install an override that wins over the
environment for the life of the process, and both the benchmark and the parity
suite go through those instead. The environment still decides when no override
is installed, which is the field A/B path, and the parity suite now pins both
directions so the dispatch decision itself is covered -- comparing the library
against itself would pass regardless of which side the dispatcher picked.

Treat the ratios as the measurement and the absolute times as this machine's,
not yours. The box these came from carries a load average around 3 on four
cores, and the absolute figures move by 10x with it while the ratios hold to
within a few percent across runs.

## The pipeline cache

**Read this before assuming shaders are your problem.** They usually are not.

The client keeps its own pipeline cache at
`~/.cache/mocktail/graphics/shadercachevk.bin` and passes the contents to
`vkCreatePipelineCache` as `initialData`. The libc shim maps the guest's
`/data/user/0/com.roblox.client/shadercachevk.bin` onto that host path, so the
blob survives between launches on its own. A recorded trace of a 78 s session
in an experience showed:

| slice | count | total |
| --- | --- | --- |
| `vkCreateShaderModule` | 2271 | 21.79 ms |
| `vkCreatePipelineCache` | 2 | 14.66 ms |
| `vkCreateGraphicsPipelines` | 130 | 3.54 ms |
| `vkCreateComputePipelines` | 2 | 0.06 ms |

That is 40 ms of pipeline and texture work in total, 0.05% of the session, and
every one of the 132 pipeline creates already passed a cache handle. The two
`vkCreatePipelineCache` calls each report `initial_bytes` around 7.2 MB, which
is the engine reading that file back.

If you are chasing a hitch, check those numbers in your own trace before
working on graphics. In the run above, frame interval p50 was 16.7 ms and p99
was 89.6 ms; 33 frames of 3490 exceeded 100 ms, and those cost 15.5 s of the
77.5 s session. The worst clustered near exact second multiples, and inside
the largest 1825 ms gap the adapter did almost nothing — presents stopped and
resumed, with only the engine's own 347 ms main-thread step in between. A
freeze with no adapter work in it is not a graphics problem.

### The fallback store

Because the client seeds its own cache, the adapter's own store normally has
nothing to do. It exists for a client that does not seed one. When the engine
passes its own `pInitialData`, the adapter leaves it alone and does not adopt
the cache, so it never overwrites data it did not produce. Startup says which
case you are in:

```
  [vulkan] pipeline cache skipped: caller-supplied initial data
```

`building` or `reusing` means the store is in charge: `building` is a first
session with nothing stored, and `reusing` means its blob was handed to the
driver.

Where the store does apply, the file name is keyed on the GPU, the driver
version, and the Roblox version, so a driver or Roblox update lands on a new
key and starts fresh rather than handing the driver a blob it cannot use.
Blobs are capped at 64 MiB and carry a checksum; one that fails either check
is discarded rather than retried on every launch.

Three settings, all in `config.yaml` under `graphics`:

- `pipeline_cache: on` (default) reads and writes the store when the client
  does not seed a cache itself.
- `pipeline_cache: clear` deletes the stored blob and runs that one session
  uncached, which is the repair to reach for if a place ever comes up wrong.
- `pipeline_cache: off` never reads or writes one.

`MOCKTAIL_PIPELINE_CACHE_MODE` sets the same thing from the environment, and
`MOCKTAIL_PIPELINE_CACHE_DIR` moves the directory.

## Frame rate and present mode

`graphics.frame_rate_limit` (default `-1`) leaves Roblox's own in-game
framerate cap in charge; Nightcap sets no `DFIntTaskSchedulerTargetFps`
override. A fixed value forwards that number to the scheduler.

`graphics.vsync` (default `auto`) selects the presentation mode. `auto` and
`on` both pick the lowest-latency synchronized mode the driver offers, in
order: `FIFO_LATEST_READY`, `MAILBOX`, `FIFO_RELAXED`, `FIFO`. The first two do
not block the render thread on the display refresh, so the frame rate is
governed by the in-game cap rather than by presentation. `off` selects
`IMMEDIATE` and does not synchronize at all.

If the frame rate is higher than your refresh rate, the extra frames are work
you cannot see. Lower the in-game cap to your refresh rate first.

## Measuring

Shift+F4 in game shows Roblox's own scheduler jobs, with a millisecond cost and
a percentage for each. It does not show Nightcap's threads — the Vulkan
adapter, the ETC2 decode workers, the libc shim — so it is a reading on the
game, not on the process.

For the whole process:

```bash
top -H -p $(pgrep -x mocktail)      # per-thread CPU, sorted
ps -L -o psr=,pcpu=,comm= -p $(pgrep -x mocktail)   # which core each thread is on
nvidia-smi --query-gpu=utilization.gpu,temperature.gpu,power.draw --format=csv
```

A thread pinned near 100% while the frame rate is steady is a busy-wait. The
one exception is the host main thread in `tpause` mode, which the OS reports
as busy while the core is parked (see above). Load spread across many
`RBX Worker` threads is the game's own scheduler, and its cost tracks the
place you are in — part count, moving parts and joints all show in the
Shift+F4 World section.

`--profile <file>` writes a Chrome trace of the Vulkan adapter's work for
ui.perfetto.dev. See [BENCHMARKING.md](BENCHMARKING.md).
