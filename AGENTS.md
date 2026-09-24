# astable — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **source** (`AT01`, `SW Astable`) for Resolume
Arena/Avenue that simulates six 555 timers at the component level and patches
them into the X, Y and brightness of a television's deflection yoke. C++17 +
GLSL 4.10, CMake, universal macOS `.bundle` and a Windows `.dll`. MIT,
intended home `github.com/stoatworks-labs/astable`.

`CLAUDE.md` is the command reference. This file is the *why*.

---

## The one idea

**The picture is where the beam went.** Nothing is drawn as a shape, and there
is no code anywhere that draws a dot, a line or a curve.

What exists is a capacitor charging through a resistor, two comparators
watching it, a flip-flop between them, and a renderer that deposits a fixed
quantum of energy per sample interval and spreads it over the distance the beam
covered in that interval. Everything a viewer sees is a consequence of those
two facts, and the harness measures the consequences rather than asserting the
shapes.

### What falls out of it

- **Four dots**, from two square waves. The beam parks at a rail for most of a
  cycle and crosses to the next in a single interval, so the corners get the
  light and the crossings are faint. Nothing decided to draw four dots; the
  dwell law did.
- **Lines**, the moment an RC goes on an output — and they are brightest at the
  end where the exponential is slowest, for the same reason.
- **Curves**, from the capacitors instead of the outputs, because a capacitor
  charging is an exponential.
- **The crawl.** Two timers a capacitor's tolerance apart are never phase
  locked, so the figure precesses at the beat. This is the charm of the whole
  thing and it is also a *measurement* problem — see the `--dots` trap below.
- **FM and duty modulation**, from one timer's capacitor into another's pin 5,
  because pin 5 *is* the threshold the comparator uses.
- **Rounded corners at speed**, from the yoke's L/R lag.

### What does NOT fall out, and is the honest limit of v0.1.0

**Nothing here has met a real 555.** The model follows the datasheet and the
measurements agree with the datasheet to 0.01%, which is a statement about
internal consistency and not about a part on a breadboard. A real bipolar 555
has a temperature coefficient, a trigger comparator with hysteresis and offset,
a discharge transistor that saturates rather than shorting, and a supply that
sags when six of them switch at once. None of that is modelled. The one place
the real part's asymmetry *is* modelled is the output stage — 0.1 V to
Vcc − 1.7 V — and that is only there because it visibly moves the figure off
centre, which is why the amplifier is capacitor-coupled.

**Six channels, and only three signals reach the yoke.** X, Y and Z. The other
three timers exist to be CV sources, which is what a sixth timer on a
breadboard is for, but there is no summing in the patch bay: the spec floated
`Ch n + Ch m` sum entries and they are not here, because the enum was already
nineteen entries long per source and a sum would have made it fifty-five.

---

## The traps

Ordered by how much time they cost here.

### A sampled comparator is a 1% error, and it is the one this plugin claims

The obvious implementation of an astable is: step the capacitor by `dt`, then
test whether it has crossed the threshold. That is wrong by up to one sample
**every cycle, always in the same direction** — the crossing is detected late,
never early — so it does not average out. At the engine's 96 samples per period
that is a systematic 1% error on the period and the same again on the duty,
which is precisely the tolerance `--period` asserts.

`Timer555::Step` therefore **solves** for the crossing instant in closed form —
`t = tau ln((vc - target) / (level - target))` — switches state exactly there,
and integrates the remainder of the interval in the new state. The result is a
period good to 0.0000% rather than 1%, and it stays exact when the engine is
running *slower than the oscillator*: `--period` drives a 2.08 µs part at 0.80
samples per period and still measures the period exactly. That case matters,
because a fast channel used as a CV source for a slow one would otherwise beat
at a rate set by the sample grid rather than by the parts.

### The capacitor discharges to ground, not to 0.1 V

A saturated discharge transistor sits at about a tenth of a volt, and modelling
that is more faithful to the part. It is also a **1.2% period error at 9 V and
2% at 5 V**, because the datasheet's 0.693 is `ln 2`, and `ln 2` is only the
answer when the discharge target is zero. The plugin claims agreement with the
datasheet formula to 1%, so the datasheet's assumption is the one that is
modelled. `kSinkVolts` is 0 and the comment says why.

### `--dots` measured three dots, and the model was right

The first version of the dots check rendered one frame and found three bright
spots with the fourth at exactly zero. That is not a bug. A frame is 16.7 ms and
the timers run at ~744 Hz, so a frame is twelve cycles during which the phase
relationship between the two channels barely moves — and at 76% duty the two
low phases simply do not overlap at most phase offsets. The fourth corner
arrives as the 7.4 Hz beat precesses, over 135 ms.

So the check accumulates over a full beat period, **on the CPU rather than
leaving it to the phosphor**, so the result does not depend on the Persistence
setting. It now reports 100% of the light in four spots with the faintest —
"both outputs low" — at 4.32%, which is close to the 0.24² ≈ 5.9% the duties
predict.

The general lesson is worth keeping: *a measurement window shorter than the
beat period cannot see the thing this plugin is most about.*

### Two harness bugs in `--yoke` that both read like renderer bugs

Measuring a coil's time constant off a sample stream went wrong twice, and
neither failure pointed at the harness.

**The midpoint crossing is not the step.** A first-order rise reaches half way
`tau ln2` *after* the step, so a baseline window placed "just before the edge"
was already 0.4 ms into the rise. That shrank the measured swing and reported
the time constant 2.2% low.

**The asymptote is not where the fit thinks it is.** The drive reaches the coil
through the amplifier's 2 s coupling capacitor, which droops the level by about
1.5% of the swing across any window long enough to measure a "rail" in. Fitting
`ln(rail − x)` against a drooping rail pulled tau 1.7% low — consistently, in
one direction, for a reason that has nothing to do with the coil.

The fix is to stop needing the asymptote. A first-order lag sampled uniformly
satisfies `x[i+1] = a x[i] + b` exactly, with `a = exp(-dt/tau)`, so regressing
each sample against the one before it recovers tau from the **slope alone** and
a slowly moving offset lands in `b` where it does no harm. That reads 0.44%.

### The preset row covers Brightness, and it has to

The first cut had presets cover "the bench and nothing else" — the six
channels, the patch bay, the yoke — on the reasoning that which television you
are watching is a different question from which breadboard. That reasoning is
sound and the result was unusable: **Curves rendered very nearly black.**

It is the plugin's own physics. The energy deposited per frame is fixed, so a
figure that parks the beam at four corners and one that sweeps it along a
continuous curve differ in peak brightness by two orders of magnitude. One
Brightness setting cannot serve both, and making the operator hunt for the
control after every preset change is not a design, it is a defect.

So `PresetColumn()` covers the contiguous bench run **plus** `PT_BRIGHTNESS`,
which is not adjacent to it. That one discontinuity is deliberate and
commented in both `Controls.h` and `Presets.h`. The rest of the Tube group
stays out.

### An option's range reads back as 0..1

`GetParamRange` on an `FF_TYPE_OPTION` returns the 0..1 the SDK gave it, not
0..count-1 — FFGL keeps an element's display slot and its stored value apart.
So `--defaults` validated preset columns against the wrong ceiling and reported
48 failures against a table that was correct, and `sweep.py` would have driven
a fourteen-entry dropdown from 0 to 1. Both now carry their own element counts,
mirrored from `Controls.h`.

### The sweep's slow channels

Seven controls read as dead in the first sweep: channels 4 and 6 are LFOs
(7.4 Hz and 2.2 Hz, there to be CV sources) and eight frames is 133 ms, a
fraction of one of their cycles. A frequency change has nowhere to show. The
sweep pins those two to a fast capacitor for its own duration, which asks the
question the sweep is actually asking — *does this control reach the picture* —
rather than the one it is not.

`Z Source` was the same story: the top of its dropdown is Ch6 Filtered, and
over eight frames Ch6's output never goes low, so blanking on it is
indistinguishable from not blanking.

### Driving Arena on win-lab: two traps that cost the whole run

Both were hit on 2026-09-21 and both look like the plugin's fault at first.

**An ssh session on Windows has no desktop.** It lands on the service window
station, so an Arena started from it sits at about 31 MB doing nothing, cannot
open a GL context and cannot be screenshotted. Arena has to be launched into the
console session (session 1) through the scheduled-task wrapper
`C:\arena-lab\s1.ps1`. There is no "it failed to load the plugin" in this
failure mode — there is just no Arena.

**Arena's REST API is good for reading and useless for instantiating.**
`/api/v1/sources` and `/api/v1/effects` list plugins by **`idstring`**, which is
the FFGL id (`AT01` here), not the display name — so search for the id, not for
`SW Astable`. But the add-effect endpoint returns **200 with nothing added**: the
clip's effect list is unchanged afterwards. Instantiation has to be driven from
Arena's own browser (the Sources tab for this plugin, double-click in the effects
browser for the effect-shaped siblings), and the proof that it happened is the
plugin's own diag log plus the preview, not the API's response.

### The SDK's traps, all inherited and all still live

These were found by tinsel, vectrix, graticule and resolume-scopes first; the
renderer copied here already works around them, and anything new must too.

- `ffglex::ScopedFBOBinding` restores the framebuffer and **not the viewport**.
- Every `ffglex::Scoped*` binding **clears to 0** rather than restoring, so
  allocating an FBO silently unbinds your input texture. `ScopeBuffer::Ensure`
  saves and restores it.
- `FFGLScopedFBOBinding.h` is not in the umbrella header. Include it by hand.
- `ffglex::FFGLFBO::Release()` leaks the colour texture.
- `SetParamInfo` clamps a STANDARD default into 0..1 *before* `SetParamRange`
  can widen it, which is why every ranged control here is 0..1.
- A display-only TEXT parameter **without** a `SetTextParameter` override makes
  `FF_INSTANTIATE_GL` fail for the whole plugin, because the SDK sets every
  default on a fresh instance and deletes it if any set returns FF_FAIL. Every
  offline harness passes happily while no host can load the plugin.
- `CFFGLPlugin::GetParameterDisplay` dereferences `m_pPlugin`, which is null
  outside a host, so the harness segfaults if you delegate to it. The plain
  branch here is deliberately self-contained.
- `astable_core` is an **OBJECT** library. In a STATIC archive the linker may
  drop `CFFGLPluginInfo`'s translation unit, giving a bundle that loads,
  exports `plugMain`, and reports no plugins. `oxbow selftest` is the only
  check in `verify.sh` that would catch it.

---

## Shape of the code

    source/Controls.{h,cpp}   the parameter ids, the preset coverage, and
                              0..1 -> ohms, farads, seconds. The Maddi
                              mark/space pot lives here.
    source/Presets.h          seven breadboards, built by a constexpr helper
                              so a row cannot put a slider value in a dropdown
    source/Astable.{h,cpp}    the plugin: declaration, the preset override,
                              the audio fold, the per-frame sequence
    source/signal/Timer555.*  one 555, with solved comparator crossings
    source/signal/Bench.*     six of them, the patch bay, the yoke
    source/signal/Signal.h    the Sample{x,y,z,dt} contract      ] copied from
    source/signal/Clock.*     ms-or-seconds detection, dt clamp  ] vectrix
    source/render/*           the energy-conserving beam renderer ]
    tools/attest/             the offline harness, including --pipe
    tools/sweep.py            no control is silently dead
    tools/verify.sh           all of it
    demo/plugin.js            the browser demo: the nine GLSL fragments copied
                              verbatim, plus a JS port of the whole engine
    demo/tools/               check_shaders.py — the two shader copies agree
    demo/vendor/              the shared kit, copied in by sync.sh. Do not edit

### The engine rate is chosen from the parts

A 555 at 1 Hz and one at 50 kHz cannot share a sample rate that suits both, so
`Bench::SetParams` picks it per frame from the fastest *running* channel: 96
samples of its period, clamped to 24–384 kHz. Above the cap the exponential is
under-resolved — the picture goes smooth where it should have corners — but the
timing does not drift, because the crossings are solved rather than sampled.
Changing the rate between frames costs nothing: the timers' state is a voltage,
not a sample index, and `dt` travels in every sample.

The block covers the frame **exactly**: sample 0 is the state the last block
ended in, un-stepped, and samples 1..n-1 each follow one step. So the n-1
intervals the renderer draws add up to one frame of engine time and there is no
gap or double-draw at the boundary. vectrix steps every sample instead and runs
1/(n-1) fast; do not copy that back.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on one M4 Max (macOS 26.4)** — `tools/verify.sh`,
22 checks, 0 failures:

- **The timers are 555s.** Ten (Ra, Rb, C) triples from 120 Hz to 34 kHz, duties
  from 50.5% to 99.8%: period and duty both within **0.01%** of
  0.693 (Ra + 2Rb) C and (Ra + Rb) / (Ra + 2Rb), measured from the edges the
  *yoke* sees rather than from the flip-flop, so a correct comparator feeding a
  wrong patch bay would still fail. The part extremes are exact against the
  flip-flop's clock, including one driven at 0.80 samples per period.
- **The capacitor swings between V5/2 and V5** to 0.04%, at rest and with pin 5
  driven to Vcc/2 and 5Vcc/6 — i.e. the CV pin really is the threshold.
- **Mark-Space moves duty and not frequency**: 26% → 89% with the period at
  0.000%.
- **Four dots**: 100% of a frame's light in four spots over a full beat period.
- **The yoke is a first-order lag**: fitted tau 0.44% out, 63.40% at one tau.
- **Brightness follows dwell**: 0.0103% spread over a 100:1 range of speed
  (vectrix's own check, on vectrix's renderer).
- **No dead controls**: all 82 swept parameters change the picture; 5 skipped
  with reasons.
- **The bundle is universal and registers**: `lipo` reports `x86_64 arm64`,
  `nm` finds `_plugMain`, and `oxbow selftest` instantiates it and lights
  21,293 pixels — reported as `SW Astable` / `AT01` / source.
- **Cost**: 0.377 / 0.394 / 0.871 ms per frame at 720p / 1080p / 4K. macOS only.

**Verified in Resolume Arena 7.27.1 on Windows, 2026-09-21** — on **win-lab**, an
x64 Windows 11 Pro VM with no GPU, so OpenGL is **Mesa llvmpipe** dropped in
beside Arena (`renderer=llvmpipe (LLVM 22.1.8, 256 bits)`, `4.5 (Core Profile)
Mesa 26.2.0`). The DLL is cross-compiled x64 in the Parallels guest on this Mac
(ARM64 Windows 11, MSVC 2022 Build Tools, `cmake -A x64`, vcpkg triplet
`x64-windows-static-md`) — 412,672 bytes, and `dumpbin /EXPORTS` shows
`plugMain`.

- **Arena registers it.** Its REST API lists `SW Astable` among 24 video sources
  under `idstring` `AT01`, with the declared description.
- **Arena loads the DLL**: `plugin loaded build=<stamp>` in the diag log under
  `%LOCALAPPDATA%\astable\`, matching the DLL built minutes earlier.
- **Arena instantiates it and the shaders compile.** Instantiated from the
  **Sources** tab, it created a clip and Arena drew its inspector with the
  component values in it, and the preview monitor showed **the four dwell dots**
  — the default preset visibly correct in the host. That is a screenshot of the
  expected picture, not a measurement.
- **The host clock unit detection was exercised by a real host and got both
  cases right.** The same build logged `host clock scale 1.0 (seconds)` under
  oxbow and `host clock scale 0.001 (milliseconds)` under Arena (Arena's raw host
  time was ≈ 574,073 at the time of the run). This is the cleanest real-host
  finding this plugin has, and the first time `source/signal/Clock.*` has met a
  host that is not ours. Note that the sibling repos' guards on the same detector
  differ — rosette's strict one falls back to the wall clock offline — so do not
  assume a change here is safe there.
- **`oxbow selftest` on x64 Windows**: 120 frames, gl error 0x0, **PASS**, 35,482
  of 921,600 pixels lit (3.9%). The diag log has no WARN, ERROR or FAIL.

**Assumed, or not yet done:**

- **Never run on a GPU in Resolume, and never instantiated in Arena on macOS.**
  Everything in the host was llvmpipe, a software rasteriser, and **nothing was
  timed on Windows** — do not turn the Windows run into a performance claim.
- **Nothing long was exercised in the host**: no long session, no composition
  save and reload, no preset recall in Arena. Whether eighty-seven controls in
  eleven groups is *usable* in the inspector, rather than merely present, is
  still untested, and so is whether the preset override reads sensibly to an
  operator.
- **Never checked against a real 555.** See "what does not fall out", above.
- **The Windows DLL that ran in Arena was not CI's.** CI builds x64 Windows on
  every push and the release workflow builds it again on a GitHub runner — both
  have run and passed, so the CMakeLists' GLEW path is now evidence for that
  route too — but the DLL put in front of Arena was cross-compiled by hand in
  the guest, and CI's has never been in front of a host.
- **No audio has arrived from a host.** It was loaded in Arena and no real audio
  reached it there either. The bin count and the `sqrt` on the magnitudes come
  from regauss and vectrix rather than from a measurement here; `--audio` injects
  a flat spectrum and only proves the path is connected. Resolume's 64-bin FFT
  mapping is still assumed, not measured.
- **The phosphor figures for P4 and P22 are from published JEDEC data**, but
  P22-as-one-white is an approximation with no equivalent in a real set: a
  single-beam model cannot have a shadow mask, so what is modelled is the white
  the three phosphors make together, keeping only the red's millisecond lag.
- **No OpenFX port.** Not in scope for 0.1.0. The browser demo came later; see
  *The browser demo* below.
- **No user guide**, so `StoatworksAbout.h` carries `guide=""`. That header is
  **generated** by `sync-about.py` now — the project is registered in the
  website's `projects.json`, in that script's TARGETS and in
  `attributions/names.json` — so do not hand-edit it. `ATTRIBUTIONS.md` is still
  a provisional hand copy, because `sync-attributions.py`'s master lists do not
  know this repo yet.

---

## The browser demo

`demo/` is the page at **astable-demo.stoatworks-labs.com**. It is a *port*, not
a recording and not the plugin, and the distinction is the whole reason the
directory is allowed to exist:

- **The shaders are the plugin's.** The nine constants in `demo/plugin.js` are
  the nine `R"(...)"` bodies in `source/render/shaders/`, copied across unedited
  and assembled the way `Prelude.cpp` assembles them.
  `demo/tools/check_shaders.py` compares them character for character —
  comments included, because the comments in this repo carry the measurements
  that justify the code — and `tools/verify.sh` runs it. **Two copies of a
  shader is exactly the arrangement that drifts**, and a demo that renders a
  *plausible* picture looks exactly like one that renders the right one.
- **The engine is a full port.** `Timer555`, `Bench`, `Phosphor`, `Tube`,
  `Controls` and the seven preset rows, all six channels, at the plugin's own
  rate. It runs at about 50 fps at 1280×720 on an M4 Max, so there was no reason
  to port fewer than six. **Nothing checks this port but a reader** — `attest`
  drives the C++ and has no idea the page exists.

### Decisions the page made, and why

- **No clip picker and no file input.** A source declares zero inputs and the
  glass shader's `HasClip` is 0, so `ClipTexture` is never read. The shared kit
  builds both controls for every demo; the page removes them from the DOM after
  mounting. A control that is present and inert is a worse answer than one that
  is absent. If a second sourceless plugin joins the kit, that is the moment to
  teach `demo.js` about it rather than repeating this.
- **`demo.blurb` is set.** The stock banner says the page runs "on generated
  clips", which for a source would be the banner itself making the kind of claim
  the banner exists to prevent.
- **Nothing audio.** The `Audio` FFT buffer parameter and `Audio Gain` are
  absent — there is no Resolume FFT in a browser, and asking a visitor for a
  microphone to demo a video source is not a trade worth making. The `Audio`
  element stays in every channel's CV Source dropdown, because it is the
  plugin's element list and the value is its index; it reads as a permanent
  silence and says so.
- **`Preset` is the plugin's override, not the kit's preset menu.** The kit
  offers a `demo.presets` dropdown that *writes* values. The plugin's is an
  override applied at read time, so the sliders show one thing while the bench
  runs another — which reads as a bug and is the host's fault, not the
  plugin's. The page reproduces `Effective()` rather than papering over it.
- **Restart is a power cycle.** The kit's transport puts `time` back to zero,
  and time running backwards is the one thing a deflection amplifier cannot do,
  so the page takes it as `Clock::Reset` + `Bench::Reset` rather than as a very
  long frame.

### What it is not evidence about

GLSL ES 3.00 in WebGL2, not desktop GL 4.1 core; a browser's clock, not a
host's; and nothing on the page measures anything. `tools/attest` is the reason
to believe the model, and it is named on the page as such.

## Relationship to vectrix

They look like the same plugin and they are not, though they share a renderer.

| | vectrix | astable |
|---|---|---|
| the signal | a function generator into fourteen guitar/eurorack effects | six 555s on a breadboard |
| the controls | frequency, ratio, mix, feedback | resistors, capacitors, a supply voltage |
| the tube | a lab scope, round, underscanned, P31 | a portable television, 4:3, overscanned, P4 |
| what you tune | the sound of a signal chain | the parts in a circuit |

`source/render/` is copied from vectrix, unchanged apart from two added
phosphors and the namespace. It was copied rather than re-derived on purpose:
the property it exists to guarantee — energy conservation, so `1/v` falls out
instead of being applied — would be true of neither copy by construction if
there were two derivations. A fix here belongs upstream too.

## Conventions

Tabs. British spelling in prose. Comments explain *why*, and especially what
goes wrong — a comment that restates the code earns nothing.
