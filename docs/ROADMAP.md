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

The cause is now known: the engine's main thread blocks rather than computes.
Over a 78 s session, 22.09 s was spent inside
`nativeCallMessagesFromMainThread` and 0.38 s of that was CPU. The ten longest
steps were all at 0% CPU, the worst at 347 ms.

What it blocks on is still open, and that is the work. `MOCKTAIL_TRACE_PUMP_STALLS=1`
prints the stack underneath a wait, which is the only way to tell a guest mutex
from a socket read from a condition variable. The near-exact second multiples
in the freeze times point at a timeout or a poll interval rather than work, so
the candidates are a blocked socket read, a server-side join wait, and a retry
backoff in the engine's join path.

- Run the same scenario with `MOCKTAIL_TRACE_PUMP_STALLS=1` and read the stack.
  This is the first thing to do and it may end the item.
- Three runs per scenario before claiming anything, per
  [BENCHMARKING.md](BENCHMARKING.md). One run is not a result.
- Whatever it turns out to be, it is upstream engine behaviour reached through
  the guest, so a fix may be a workaround rather than a change.

Done when a fresh experience no longer freezes for a second at a time.

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
