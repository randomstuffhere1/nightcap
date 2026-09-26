# Nightcap roadmap

What Nightcap is working toward, in priority order. Items move to
[Shipped](#shipped) when they land on `main`. Open an issue to propose a
change to this list.

## 1. Settings UI

Nightcap is configured by editing `~/.config/mocktail/config.yaml`. Most
people never open it, so they never find the FPS unlock or the vsync switch.

Goal: a small GTK4 settings window, reachable from the app menu and from a
`--settings` flag, that reads and writes the same `config.yaml`.

- Graphics tab: backend, frame rate limit, vsync, small texture upscale.
- Input tab: the `input.raw_mouse` toggle and controller options.
- Advanced tab: FFlag overrides with a plain text editor for `fflags.json`.
- Discord tab: every field in the `discord_rpc` block, so a friend can see
  what you are playing and you can switch it all off in one click.
- Changes apply on next launch. A restart button restarts Nightcap.

Done when every option in `config/mocktail.example.yaml` that a player would
touch can be set without a text editor.

## 2. Controller button glyphs

Upstream added Xbox and PlayStation gamepad support through SDL. Controllers
already hot-plug and release their held inputs when unplugged. One rough edge
is left.

- Correct button glyphs for the connected pad. `SDL_GetGamepadType` is only
  read for Xbox and PlayStation pads, so a Switch Pro, a GameCube pad, or any
  generic pad SDL reports as `SDL_GAMEPAD_TYPE_STANDARD` reaches Roblox as
  type 0 and draws generic glyphs. `SDL_GetRealGamepadType`, and the vendor
  and product IDs, are available and unused.

Done when a plugged-in pad shows its own buttons.

## 3. Load and join freezes

**Investigated. No host-side cause found; probably not actionable from here.**

The freezes are real — a 78 s session lost 15.5 s across 33 frames, and a 35 s
one lost 13.4 s across 32 — but every layer instrumented says the engine is
*idling*, not blocked. Full numbers in
[PERFORMANCE.md](PERFORMANCE.md#when-the-engine-stops-rendering); the short
version:

- The main thread pumps at a constant 4.17 ms through a freeze, the same as
  when frames are flowing, at 7.5% busy against 9.0%.
- The render thread drops to 0.2% busy, from 3.2%. It has nothing to do.
- Under `strace`, 141 s containing multi-second freezes produced three network
  reads over 50 ms, all at startup. Nothing waits for anything close to a
  freeze duration. There were 21,616 `EAGAIN` returns from non-blocking reads:
  polling, not waiting.
- Gap durations cluster near exact second multiples, mean 998.7 ms, which reads
  like a retry or poll interval.

The engine is calling into itself on schedule, finding no work, and not
rendering. That is most likely engine or server-side behavior reached through
the guest.

Two things were chased and disproved, recorded so they are not chased again. A
shader-compilation cause was ruled out first: all pipeline and texture work
totalled 40 ms in a 78 s session. Then a DNS storm looked conclusive —
`inet_addr` called 12,408 times for one address, 808 times in a second during
the largest gap — but per-gap coverage showed fourteen of the ~1 s gaps contain
essentially none of it, and the session-wide correlation is r = −0.07. It
explains one gap and nothing else.

- Not a Nightcap-side fix, on the evidence so far. If someone wants to pursue it
  further, the remaining lead is engine-internal, and `perf record -g` is the
  only tool left that has not been tried: it samples every thread, so unlike
  `--profile` it can see a spin that emits no Vulkan or pump slices, and unlike
  `strace` it does not perturb per-syscall cost.
- Do not read a low `cpu_us` on a pump slice as a blocked thread without also
  checking the cadence. An idle polling thread and a blocked one look the same
  on CPU time alone, and that misreading is what sent this investigation down
  two wrong paths.

## Why item 3 was rewritten

This item was written as "first-load stutter" and assumed shader compilation
was the cause. A recorded trace says otherwise. Over a 78 s session in an
experience, all pipeline and texture work in the adapter together came to
40 ms, which is 0.05% of wall time:

| slice | count | total |
| --- | --- | --- |
| `vkCreateShaderModule` | 2271 | 21.79 ms |
| `vkCreatePipelineCache` | 2 | 14.66 ms |
| `vkCreateGraphicsPipelines` | 130 | 3.54 ms |

All 132 pipeline creates already passed a cache handle, and the two
`vkCreatePipelineCache` calls report `initial_bytes` of about 7.2 MB: the
client loads its own `shadercachevk.bin` and passes it to the driver. The
shader cache was working. Compiling shaders is not the stutter.

The stutter is elsewhere. Frame interval p50 was 16.7 ms and p99 89.6 ms, and
33 frames of 3490 ran over 100 ms — 0.9% of frames but 15.5 s of lost time
out of 77.5 s. The worst of them cluster near exact multiples of a second:

```
1824.6ms   1415.1ms   1354.0ms   1001.4ms   1000.0ms
 999.9ms    998.6ms    994.0ms    945.5ms
```

Inside the largest 1825 ms gap the adapter did almost nothing: presents stop
and resume, and the only large slice is the engine's own main-thread step at
347 ms. Comparing that step's `cpu_us` against its duration showed it was
blocked, not busy, which is what the item above now says.

## Shipped

- A pipeline cache fallback. The client already persists its own
  `shadercachevk.bin` and hands it to `vkCreatePipelineCache`, so this does
  nothing on a stock client; it only engages if a future client stops seeding
  the cache itself. `graphics.pipeline_cache` turns it off, or clears it for
  one session. See [PERFORMANCE.md](PERFORMANCE.md).
- Lower input latency: frames are presented as soon as they are ready.
- Controllers hot-plug without a restart, and a pad that is unplugged mid-game
  releases its held buttons and sticks instead of leaving them stuck.
- Mouse look keeps responding after a long turn, instead of going dead until
  the pointer travels back from past the screen edge.
- Optional raw mouse input: `input.raw_mouse` in `config.yaml` sends look
  deltas in host units, so fractional display scaling no longer changes
  first-person sensitivity.
- Small textures upscaled 4x by default, with PNG overrides in
  `~/.config/mocktail/textures`.
- ETC2 textures decoded on desktop GPUs without stutter.
- No more error 319 kicks after joining a server.
- Roblox sees the real host RAM and screen size.
- Desktop launcher that works on Wayland and finds the installed binary.
- Discord Rich Presence on by default, with the experience name, creator,
  elapsed time, the experience icon with a Roblox badge, and a join button
  when the server is public. See the [FAQ](../FAQ.md) to switch it off.
- Custom Discord presence: title, details, state, and large and small icons
  from `config.yaml`, with `{place_name}`, `{place_icon}`, and
  `{creator_name}` placeholders.
- `--profile` writes a Chrome-format trace of the Vulkan adapter's work,
  including shader and pipeline creation, readable in `ui.perfetto.dev`.
  [BENCHMARKING.md](BENCHMARKING.md) fixes the experiences to measure and
  `scripts/summarize_profile_trace.py` builds the before and after table.
- Texture uploads no longer stall the frame they land on: ETC2 decode and
  resampling run on a persistent worker pool.
- The host main thread rests between engine polls instead of spinning, and
  GameMode's core pinning is released, so Roblox's workers spread across
  every core. See [PERFORMANCE.md](PERFORMANCE.md).
- Own app ID `io.github.CoderDayton.nightcap`, so Nightcap installs next to
  upstream.
- AppImage and signed Flatpak releases on every `v*` tag, with the Flatpak
  repo on GitHub Pages.

## Not planned

- Replacing the `mocktail` binary and config names. Keeping them makes
  upstream merges clean.
- DEB, RPM, and AUR packages. Upstream ships those for unmodified Mocktail.
