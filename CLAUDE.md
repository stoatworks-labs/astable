# astable

Six 555 timers into a television's deflection yoke, as an FFGL **source** for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. MIT.

Read `AGENTS.md` before changing the 555 model, the patch bay or the preset
coverage.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build`
- Render a frame offline: `./build/attest --out /tmp/f.png --size 1920x1080 --preset 4`
- Set anything by name: `--set "Ch1 Ra=0.4" --set "X Source=7"` (repeatable)
- List parameters, with the units the host is shown: `./build/attest --list`
- Inject audio the host would have sent: `--audio 0.8`
- Raw frames for the video pipeline: `./build/attest --pipe --width 1920 --height 1080 [--frames N] [--fps 60] [--script cues.txt] | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - out.mov`
  (a **source** reads nothing, so `--pipe` has no stdin side; with no `--frames`
  it writes until the reader hangs up. `--script` is `frame  Parameter Name  value`,
  the fleet's format, and refuses a name that is not a parameter.)

## Verify
- Everything: `tools/verify.sh` (fresh universal build + 23 checks, ~11 s)
- The demo's shaders are still the plugin's: `python3 demo/tools/check_shaders.py`
- The datasheet formula: `./build/attest --period`
- The capacitor's swing against pin 5: `./build/attest --swing`
- The mark/space pot moves duty, not period: `./build/attest --markspace`
- Two squares into X and Y are four dots: `./build/attest --dots`
- A coil's step response: `./build/attest --yoke`
- Brightness is independent of beam speed: `./build/attest --energy`
- Presets render, differ, and row 1 = the defaults: `./build/attest --presets --defaults`
- No name over 16 characters: `./build/attest --names`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/attest --bench`
- Registers in a real host: `~/Projects/resolume/oxbow/build/oxbow selftest build/Astable.bundle`

## Notes
- **The picture is where the beam went.** Nothing is drawn as a shape. If you
  are tempted to draw one, either it already falls out of the circuit or the
  circuit is wrong somewhere and that is the bug.
- **`Timer555::Step` solves its comparator crossings, it does not sample them.**
  A sampled comparator is up to a sample late every cycle, always in the same
  direction, which is a 1% period error at 96 samples a period — exactly the
  tolerance the plugin claims. Do not "simplify" this to a threshold test.
- **The capacitor discharges toward ground, not toward a saturated transistor's
  0.1 V.** The datasheet's 0.693 assumes ground; a tenth of a volt puts the
  trigger crossing 1.2% early at 9 V.
- **`dt` lives in the sample**, not in a block-wide rate. That is what makes
  brightness independent of the engine rate as an identity.
- **Sample 0 of a block is the previous block's last state, un-stepped**, so
  the n-1 intervals cover exactly one frame. vectrix does this differently.
- **A parameter change must NOT reset the timers** — the opposite of the
  fleet's GPU habit. `Bench::SetParams` is called every frame.
- **The engine rate is chosen per frame from the fastest running channel**
  (96 samples a period, clamped to 24–384 kHz). Changing it costs nothing: the
  timers' state is a voltage, not a sample index.
- **The yoke amplifier is capacitor-coupled** (2 s), because a 555's output is
  not symmetric about Vcc/2 and a DC-coupled amplifier would put the figure off
  centre and move it when Vcc changed.
- **Presets are an OVERRIDE, not a write** — Resolume does not consume value
  events. `Effective()` is the one place that reads them.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it, so every ranged control is 0..1 and the units live in `Controls.cpp`.
- **An option's range reads back as 0..1 whatever its element count**, so never
  drive a dropdown from `GetParamRange`. The sweep has its own table.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no
  host can instantiate the plugin at all.
- `astable_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `AT01`; display name `SW Astable` (16 chars, not null-terminated).
- `sample`, `input`, `output`, `filter`, `common`, `active`, `half`, `patch`
  are GLSL reserved words. Shader errors surface only at runtime, in the log.
- `source/render/` is **vectrix's renderer, copied**. Fixes belong upstream
  too — see ATTRIBUTIONS.md.
- `demo/` is the browser demo at astable-demo.stoatworks-labs.com: the plugin's
  own nine GLSL fragments copied verbatim plus a JavaScript port of the whole
  engine. `demo/vendor/` is the shared kit — do not edit it; it is copied in by
  `stoatworks-backend/resolume-demo/sync.sh astable`. Deploy from the repo root
  with `cf-run npx wrangler deploy`, and verify by CONTENT: a wrong page still
  answers 200.

## Not done yet
- Registered, loaded and instantiated in Resolume Arena 7.27.1 on Windows
  (2026-09-21) on Mesa llvmpipe, with the four dwell dots visible in Arena's
  preview. **Never on a GPU in Resolume, never in Arena on macOS**, and nothing
  was timed on Windows — the ms/frame figures are macOS-only. No long session,
  no composition save/reload, no preset recall in the host.
- Never checked against a real 555.
- The Windows x64 DLL that ran in Arena was cross-compiled by hand in the
  Parallels guest (412,672 B, `plugMain` exported). CI builds x64 Windows itself
  now and has passed; that build has never been in front of Arena.
- Driving Arena on win-lab: an ssh session has no desktop, so launch Arena via
  the session-1 scheduled task `C:\arena-lab\s1.ps1`; Arena's REST API lists
  plugins by `idstring` (`AT01`) and its add-effect endpoint returns 200 without
  adding anything, so instantiate from Arena's own browser. See `AGENTS.md`.
- No user guide, no OpenFX port, no video.
- The browser demo is live but has only been driven on macOS Chrome. It is a
  port and not a measurement: WebGL2 gives GLSL ES 3.00, not desktop GL 4.1
  core, and nothing on the page checks anything.
- `ATTRIBUTIONS.md` is still a provisional hand copy — `sync-attributions.py`
  does not know this repo. `StoatworksAbout.h` is generated by `sync-about.py`
  now, with `guide=""`; do not hand-edit it.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/astable/astable.YYYY-MM-DD.log
