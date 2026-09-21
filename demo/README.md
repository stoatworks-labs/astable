# demo/ — the browser demo

Live at **<https://astable-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file and `tools/` out of the upload.

    index.html    the shell
    plugin.js     this plugin's parameters, its ported engine, its shaders
    vendor/       the shared kit, copied in by sync.sh — DO NOT EDIT
    tools/        check_shaders.py, run by tools/verify.sh
    _headers      CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

A **port**, not a recording and not the plugin.

The shaders are the plugin's, copied across unedited. The nine constants at the
top of `plugin.js` are the nine `R"( ... )"` bodies in
`source/render/shaders/`, assembled the way `Prelude.cpp` assembles them and run
in the same six passes in the same order as `BeamGeometry::Render`.
`tools/check_shaders.py` compares them character for character and
`../tools/verify.sh` runs it, because two copies of a shader is exactly the
arrangement that drifts — quietly, since a demo that renders a *plausible*
picture looks exactly like one that renders the right one.

The engine is a port, and a complete one: `Timer555`, `Bench`, `Phosphor`,
`Tube`, `Controls` and the seven rows of `Presets.h`, all six channels, at the
plugin's own rate — 96 samples of the fastest running channel's period, clamped
to 24–384 kHz. **Nothing checks the port but a reader.**

Everything else is not the plugin: no Resolume, no composition, no layer stack,
no FFGL, and GLSL ES 3.00 in WebGL2 rather than desktop GL 4.1 core. A pixel
here is not a measurement of a pixel there.

## What is deliberately absent

- **Anything audio.** The `Audio` FFT buffer parameter and `Audio Gain` are not
  on the panel. `Audio` stays in every channel's CV Source dropdown because it
  is the plugin's element list and the value is its index; it is a permanent
  silence here.
- **The clip picker and the "use my own file" button.** Astable is a source with
  zero inputs. The kit builds both for every demo and `plugin.js` removes them,
  rather than leaving a control present and dead.
- **The About block.** A text line and three link buttons exist so a *host* has
  somewhere to put them. A web page has links of its own.

## Working on it

```bash
python3 -m http.server 8931          # from this directory
python3 tools/check_shaders.py       # the copies still match the C++
../tools/verify.sh                   # everything, including the above
```

There is no build step. It is hand-written ES modules and what is committed is
what is served.

**After changing a shader in `source/render/shaders/`, copy it across here too**
— `check_shaders.py` will tell you which one and where the first difference is.
Do not edit the GLSL in `plugin.js` to make something compile in WebGL2: `port()`
in `vendor/gl.js` handles the version line and the precision qualifiers and
nothing else, and if a shader will not compile here the answer is to say so on
the page.

**`vendor/` is a copy.** Fix the kit in
`stoatworks-backend/resolume-demo/kit/` and re-run
`stoatworks-backend/resolume-demo/sync.sh astable`. A fix applied to this copy
is a fix the other seventeen repos silently do not have.

## Deploying

From the **repository root**, not from here:

```bash
cf-run npx wrangler deploy
curl -s 'https://astable-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

Verify by **content**, never by status code: a stale page returns a cheerful 200.

## Embed mode

`?embed=1` renders the output and nothing else, so the page can be a video
source — an OBS browser source, a WebLinked URL — instead of something somebody
reads. `?size=1920x1080`, `?bg=black|checker|white` and any parameter id work as
query parameters; the "Copy link" button already produces them. `?clip=` does
nothing here, because there is no clip.

    https://astable-demo.stoatworks-labs.com/?embed=1&size=1920x1080&preset=7
