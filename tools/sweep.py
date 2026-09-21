"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline, and
report any that made no difference.

    python3 tools/sweep.py

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**Most of this plugin is unpatched most of the time, by design.** Six timers
run; by default only two of them reach the yoke. Channel 3's Ra is correctly
dead until channel 3 is patched to something, so each channel's controls are
swept with that channel plugged into X. A parameter missing from CONTEXT is
swept against the defaults and will be reported dead if the defaults do not
read it -- which is the table doing its job.

**An option's range is reported as 0..1 whatever its element count.** FFGL
keeps an element's display slot and its stored value apart, and
`GetParamRange` on an option returns the 0..1 the SDK gave it rather than
0..count-1. So every option here needs an explicit `_high`; without one the
sweep would drive a fourteen-entry dropdown from 0 to 1 and report it dead
because entries 0 and 1 happen to look similar.

**A filter that nothing is listening to is dead.** `Ch1 Filter` changes
`Ch1 Filtered` and nothing else, so it is swept with X Source on *Ch1
Filtered* rather than Ch1 Output.

**Amp Rail only does something when the drive reaches it.** At the shipped
gain the drive is about half the rail, so the clip never engages -- correctly.
It is swept with X Gain wound up.

**Persistence needs a phosphor with something to remember.** P4's slow layer
decays in 26 microseconds, so at 60 fps it correctly shows no trail at all.
It is swept on P7, whose slow layer is 174 ms.

**Audio needs audio.** The harness injects a flat spectrum with `--audio`, and
the level only reaches the picture through a CV Source set to Audio.

**Never sweep the About block.** Those are buttons that open a web browser, and
sweeping them opens one tab per press.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "attest")
SCRATCH = tempfile.mkdtemp(prefix="atsweep")

WIDTH, HEIGHT = 480, 270
FRAMES = 8

# Parameters that cannot or must not be swept, with the reason.
SKIP = {
    "Audio": "an FFT buffer the host fills; its single float value is meaningless",
    "About": "a display-only text line",
    "Project page": "a button that opens a web browser",
    "Source on GitHub": "a button that opens a web browser",
    "Support the work": "a button that opens a web browser",
}

# The world every sweep starts from. Brightness up a little on the baseline so
# a control that only moves the dim parts of the picture still moves something
# the difference can see.
BASE = {
    "Brightness": 0.6,
}

# Element counts, so an option is swept across its real range rather than 0..1.
# These mirror Controls.h; a dropdown that grows and is not updated here is
# swept too narrowly rather than silently skipped.
OPTION_HIGH = {
    "C Decade": 5,
    "CV Source": 13,
    "X Source": 18,
    "Y Source": 18,
    "Z Source": 18,
    "Z Mode": 2,
    "Phosphor": 7,
    "Preset": 7,
}


# Channels 4 and 6 ship as LFOs -- 7.4 Hz and 2.2 Hz, there to be CV sources --
# and eight frames is 133 ms, which is a fraction of one of their cycles. A
# frequency change has nowhere to show in that window, so seven controls read
# as dead while working perfectly. Pinning those two to a fast capacitor for
# the duration of the sweep asks the question the sweep is actually asking --
# does this control reach the picture at all -- rather than the one it is not,
# which is whether it does so within 133 ms at its shipped frequency.
FAST_DECADE = {4: 1, 6: 1}


def channel_context(n, source="Output", sweeping=None, **extra):
    """Plug channel n into X so its own controls can reach the picture."""
    offset = {"Output": 0, "Cap": 6, "Filtered": 12}[source]
    ctx = {"X Source": offset + n, "X Gain": 0.6}
    if n in FAST_DECADE and sweeping != "C Decade":
        ctx[f"Ch{n} C Decade"] = FAST_DECADE[n]
    ctx.update(extra)
    return ctx


CONTEXT = {}

for n in range(1, 7):
    c = f"Ch{n} "
    # Ra, Rb and the capacitor move the frequency, which is visible whenever
    # the channel is patched at all.
    CONTEXT[c + "Ra"] = channel_context(n)
    CONTEXT[c + "Rb"] = channel_context(n)
    CONTEXT[c + "C Decade"] = channel_context(n, sweeping="C Decade")
    CONTEXT[c + "C Fine"] = channel_context(n)
    CONTEXT[c + "Mark-Space"] = channel_context(n)
    CONTEXT[c + "Reset"] = channel_context(n)
    CONTEXT[c + "Level"] = channel_context(n)
    # A CV source needs depth, and depth needs a source. Channel 4 is the 7 Hz
    # LFO, so its capacitor is the one that visibly sweeps another timer.
    CONTEXT[c + "CV Source"] = channel_context(n, **{c + "CV Depth": 0.9, "_high": 10})
    CONTEXT[c + "CV Depth"] = channel_context(n, **{c + "CV Source": 10 if n != 4 else 12})
    # The filter feeds "Ch n Filtered" and nothing else, so that is what X has
    # to be listening to.
    CONTEXT[c + "Filter"] = channel_context(n, source="Filtered")

CONTEXT.update({
    # The patch bay. A source is swept across every entry; the gains and
    # offsets act on whatever is already patched.
    "X Source": {},
    "Y Source": {},
    "X Gain": {},
    "Y Gain": {},
    "X Offset": {},
    "Y Offset": {},
    # Z does nothing without a mode, and a mode does nothing without a source.
    # The top of the Z Source dropdown is Ch6 Filtered, and Ch6 is the 2.2 Hz
    # LFO: over eight frames its output never goes low, so blanking on it is
    # indistinguishable from not blanking at all. Same fix as the channels.
    "Z Source": {"Z Mode": 1, "Ch6 C Decade": 1},
    "Z Mode": {"Z Source": 1},

    # The yoke. Amp Rail only bites when the drive reaches it.
    "Coil X": {},
    "Coil Y": {},
    "Deflection Gain": {},
    "Amp Rail": {"X Gain": 0.95, "Y Gain": 0.95, "Deflection Gain": 0.9},

    # The supply. Vcc changes the output swing and therefore the deflection;
    # the audio level only arrives through a CV source set to Audio.
    "Vcc": {},
    "Audio Gain": {"Ch1 CV Source": 13, "Ch1 CV Depth": 1.0, "_audio": 0.9},

    # The tube. Persistence needs a phosphor with a slow layer and several
    # frames for a trail to build.
    "Phosphor": {},
    "Persistence": {"Phosphor": 5, "_frames": 20},
    "Focus": {},
    "Face Aspect": {},
    "Corner Radius": {},
    "Overscan": {},
    "Brightness": {},

    # Value 1 is the defaults, so preset 1 against Custom provably changes
    # nothing. Swept to the last row instead.
    "Preset": {},
})


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
    return found


def render(path, overrides, frames, audio=None):
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    if audio is not None:
        args += ["--audio", str(audio)]
    merged = dict(BASE)
    merged.update({k: v for k, v in overrides.items() if not k.startswith("_")})
    for name, value in merged.items():
        args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    pid, name, low, high, context = job
    frames = context.get("_frames", FRAMES)
    audio = context.get("_audio")

    # An option's declared range is 0..1 whatever its element count, so the
    # top of the sweep comes from OPTION_HIGH by suffix.
    for suffix, value in OPTION_HIGH.items():
        if name == suffix or name.endswith(" " + suffix):
            high = value
            break
    low = context.get("_low", low)
    high = context.get("_high", high)

    lo = dict(context)
    hi = dict(context)
    lo[name] = low
    hi[name] = high

    a = render(f"{SCRATCH}/{pid}_lo.png", lo, frames, audio)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi, frames, audio)
    fraction, count = difference(a, b)
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if name in SKIP:
            skipped.append((name, SKIP[name]))
            continue
        work.append((pid, name, low, high, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
