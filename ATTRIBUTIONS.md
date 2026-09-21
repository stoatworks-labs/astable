# Attributions

Astable is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Astable is not
> registered there yet, so this copy is hand-written. Register it before release
> — and note that the script's `--only` flag truncates the file rather than
> filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL source is defined by this SDK's headers — there is
no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. Arrives inside the FFGL submodule
at `external/ffgl/deps/glew-2.1.0` for the headers; macOS uses the system
OpenGL framework instead.

## Work from elsewhere in the fleet

### vectrix — the beam renderer

<https://github.com/stoatworks-labs/vectrix>
Licence: MIT
Copyright: Stoatworks Labs

**`source/render/` is vectrix's renderer, copied rather than reimplemented**, on
2026-09-21: `BeamGeometry`, `ScopeBuffer`, `GLState`, `Phosphor`, `Tube`,
`Shaders` and all five GLSL passes (`shaders/Prelude.cpp`, `Trace.cpp`,
`Decay.cpp`, `Bloom.cpp`, `Glass.cpp`). `source/signal/Signal.h` — the
`Sample{x,y,z,dt}` contract — and `source/signal/Clock.*` come with it, as do
`Diag.*` and the About headers. Every copied file carries a header saying so.

Same author, same licence, and copying was the right call rather than a
shortcut: the thing being shared is an *energy-conserving* beam model, where a
fixed quantum per sample interval is spread over the distance the beam covered
so that `1/v` brightness falls out instead of being applied. Re-deriving that
would produce a second implementation to keep in step with the first, and the
property it exists to guarantee would then be true of neither by construction.
`attest --energy` is vectrix's own check, run here against the copy.

What astable changed in it: two phosphors added at the front of the table (P4,
the monochrome television white and this plugin's default, and a P22 white for a
colour set), and the namespace. Nothing else.

### Traps inherited from the fleet

The SDK defects worked around here — `ScopedFBOBinding` not restoring the
viewport, every `ffglex::Scoped*` clearing to 0 rather than restoring,
`FFGLFBO::Release()` leaking its colour texture, `SetParamInfo` clamping a
STANDARD default before `SetParamRange` can widen it, and a display-only TEXT
parameter needing a `SetTextParameter` override or no host can instantiate the
plugin — were all found by **tinsel**, **vectrix**, **graticule** and
**resolume-scopes** first. See `AGENTS.md`.

## Inspiration

### Ms Mad Lemon — *What A 555 Timer Looks Like On A CRT TV*

<https://www.youtube.com/@MsMadLemon>

Not code, and nothing was copied — but this plugin would not exist without the
video. The idea of driving a television's deflection yoke directly from 555/556
astables to draw vector figures "in Vectrex fashion", the follow-up with an
LM358 triangle generator, and the earlier CRT TV modification are hers. The
"Maddi mark/space" trick modelled in `Controls.cpp` — a pot from pin 6 to V+
with the wiper to pin 7, changing the duty without changing the frequency —
comes from the comments on that series.

vectrix credits the same series for the same reason.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
