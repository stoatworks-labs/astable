#pragma once

// Copied from vectrix (github.com/stoatworks-labs/vectrix, MIT, Stoatworks Labs)
// on 2026-09-21, renamed into the astable namespace and otherwise unchanged
// except where a comment below says so. The renderer is the same author's
// energy-conserving beam model; see ATTRIBUTIONS.md and AGENTS.md.
#include <cstddef>

/**
	The contract between the signal chain and the renderer.

	This header has no dependencies and includes nothing but the standard
	library, on purpose: everything in `signal/` and everything in `render/`
	includes it, and it is where the conventions that both halves must agree
	about are written down once.
*/
namespace astable
{
//---------------------------------------------------------------------------
// Conventions. Break one of these and the failure is a picture that looks
// plausible and is wrong, which is the expensive kind.
//---------------------------------------------------------------------------

/**
	One instant of the beam.

	`x` and `y` are **deflection volts**, not screen coordinates and not a
	normalised position. ±1.0 is nominal full deflection, and the chain is
	allowed to exceed it -- there is no clamp anywhere in the signal path except
	the compressor's explicit limiter. Overdriving past ±1 pushes the figure off
	the screen, which is exactly what an overdriven amplifier into a deflection
	yoke does, and refusing to model that would be refusing to model the thing.

	`z` is the **grid voltage**: beam current, 0 = fully cut off. It is not a
	colour and it is not an alpha. Making it a first-class per-sample output is
	what lets blanking between strokes, the gate cutting the beam, and retrace
	blanking all be the *same* mechanism instead of three separate special cases
	in the renderer.

	`dt` is how long this interval lasts, from this sample to the next, in
	seconds. It is carried per-sample rather than derived from a block-wide
	sample rate because it is what makes trace brightness independent of the
	sample count: the renderer deposits `BeamPower * dt * z` of energy per
	interval, so the light in a frame is

	    sum(E) = BeamPower * (frame duration) * (mean beam current)

	in which the number of samples does not appear at all. A source that emits a
	uniform `dt` gets that for free; one that puts more samples where the beam is
	slow is still exactly correct. The last sample in a block has no interval
	after it and its `dt` is unused.

	+Y is **up**. The flip into the renderer's screen space happens in exactly
	one place, in the tube pass. Oscilloscope music is authored Y-up, and a
	second flip somewhere in the signal path would render every piece of it
	upside down while looking perfectly reasonable in isolation.
*/
struct Sample
{
	float x  = 0.0f;
	float y  = 0.0f;
	float z  = 1.0f;
	float dt = 0.0f;
};

static_assert( sizeof( Sample ) == 16, "Sample is uploaded straight into a VBO as two vec4 attributes" );

/// The largest block the engine will ever synthesise in one frame. The engine
/// rate is capped at 384 kHz (see Bench.h), which at the 24 fps clamp floor is
/// exactly 16000 samples; the rest is headroom.
constexpr int kMaxBlock = 16384;

} // namespace astable
