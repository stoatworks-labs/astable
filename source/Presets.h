#pragma once

#include "Controls.h"

#include <array>

/**
	Factory presets: a whole breadboard an operator can reach in one gesture.

	Each one is the thing the video does next -- two squares into X and Y, an
	RC on one of them, the capacitors instead of the outputs, one timer's cap
	into another's pin 5 -- so it is a demonstration of one consequence of the
	model rather than a set of slider positions that happened to look good.

	**Presets are an OVERRIDE, not a write.** Resolume does not consume value
	events, so a plugin cannot push a preset's values back into the inspector;
	if it changes its own parameters the sliders keep showing the old numbers.
	So while the dropdown is on anything but Custom, the row's values are laid
	over the operator's every frame at read time, and the inspector is, for
	those columns, not the truth. Element 0 of the dropdown is Custom and is
	not in this table: it means "the controls are the truth".

	**A row covers the bench, plus Brightness**: the six channels, the patch
	bay, the yoke, and the one tube control the figure itself decides. See the
	note above `PresetColumn` in Controls.h for why Brightness is in and the
	rest of the Tube group is not -- in short, a figure that parks the beam and
	one that sweeps it differ in peak brightness by two orders of magnitude,
	because the energy per frame is the same and the path is not.

	**Row 1 is also the constructor's defaults.** `attest --defaults` fails
	when the two go out of step, which the fleet learned to test for the hard
	way (escapement shipped with defaults a retuned preset had left behind).

	**Values are host-facing.** Standard parameters hold 0..1; options,
	integers and booleans hold their real value, because `SetParamInfo`'s 0..1
	clamp is guarded by the parameter type. The builders below take 0..1 for
	the sliders and indices for the dropdowns, so a row cannot mix them up.
*/
namespace astable
{
namespace presets
{
struct Preset
{
	const char* name;
	std::array< float, kPresetParamCount > v;
};

/// One channel's ten columns, in ChannelControl order.
struct Ch
{
	float ra, rb, decade, fine, cv, depth, markSpace, reset, filter, level;
};

/// The patch bay's eight.
struct Patch
{
	float xs, ys, zs, xg, yg, xo, yo, zm;
};

/// The yoke's four.
struct Yoke
{
	float cx, cy, gain, rail;
};

//The resistors, as slider positions: log over 1 kR .. 1 MR, so p = log10(R/1k)/3.
constexpr float kR1k   = 0.0f;
constexpr float kR2k2  = 0.11414f;
constexpr float kR4k7  = 0.22403f;
constexpr float kR10k  = 0.33333f;
constexpr float kR22k  = 0.44715f;
constexpr float kR47k  = 0.55800f;
constexpr float kR100k = 0.66667f;

//Capacitor decades, as element values.
constexpr float k1nF = 0.0f, k10nF = 1.0f, k100nF = 2.0f, k1uF = 3.0f, k10uF = 4.0f, k100uF = 5.0f;

//CV sources: 0 none, 1..6 outputs, 7..12 caps, 13 audio.
constexpr float kCvNone = 0.0f, kCvCh4Cap = 10.0f, kCvCh6Cap = 12.0f;

//Patch sources: 0 off, 1..6 outputs, 7..12 caps, 13..18 filtered.
constexpr float kOff = 0.0f;
constexpr float kOut1 = 1.0f, kOut2 = 2.0f;
constexpr float kCap1 = 7.0f, kCap2 = 8.0f;
constexpr float kFilt1 = 13.0f;

constexpr Ch stock( float ra, float rb, float decade, float fine )
{
	return Ch{ ra, rb, decade, fine, kCvNone, 0.0f, 0.5f, 1.0f, 0.0f, 1.0f };
}

/// The six spare-and-default channels every row starts from. 1 and 2 are the
/// two squares at 744 and 737 Hz (a 1% capacitor mismatch, which is what two
/// parts out of the same bag do); 3 is a 2.2 kHz spare; 4 is a 7.4 Hz LFO;
/// 5 a 1.6 kHz spare; 6 a 2.2 Hz LFO. The LFOs exist to be CV sources.
constexpr Ch kCh1 = stock( kR10k, kR4k7, k100nF, 0.0f );
constexpr Ch kCh2 = stock( kR10k, kR4k7, k100nF, 0.00432f );//x1.01
constexpr Ch kCh3 = stock( kR22k, kR22k, k10nF, 0.0f );
constexpr Ch kCh4 = stock( kR100k, kR47k, k1uF, 0.0f );
constexpr Ch kCh5 = stock( kR4k7, kR2k2, k100nF, 0.0f );
constexpr Ch kCh6 = stock( kR47k, kR10k, k10uF, 0.0f );

constexpr Patch kPatchDots = Patch{ kOut1, kOut2, kOff, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f };
constexpr Yoke kYokeScope  = Yoke{ 0.0f, 0.0f, 0.5f, 0.66f };

constexpr Preset make( const char* name, Ch c1, Ch c2, Ch c3, Ch c4, Ch c5, Ch c6, Patch p, Yoke y,
                       float brightness )
{
	Preset out{ name, {} };
	const Ch chans[ kChannels ] = { c1, c2, c3, c4, c5, c6 };
	int i = 0;
	for( const Ch& c : chans )
	{
		out.v[ i++ ] = c.ra;
		out.v[ i++ ] = c.rb;
		out.v[ i++ ] = c.decade;
		out.v[ i++ ] = c.fine;
		out.v[ i++ ] = c.cv;
		out.v[ i++ ] = c.depth;
		out.v[ i++ ] = c.markSpace;
		out.v[ i++ ] = c.reset;
		out.v[ i++ ] = c.filter;
		out.v[ i++ ] = c.level;
	}
	out.v[ i++ ] = p.xs;
	out.v[ i++ ] = p.ys;
	out.v[ i++ ] = p.zs;
	out.v[ i++ ] = p.xg;
	out.v[ i++ ] = p.yg;
	out.v[ i++ ] = p.xo;
	out.v[ i++ ] = p.yo;
	out.v[ i++ ] = p.zm;
	out.v[ i++ ] = y.cx;
	out.v[ i++ ] = y.cy;
	out.v[ i++ ] = y.gain;
	out.v[ i++ ] = y.rail;
	out.v[ i++ ] = brightness;
	return out;
}

static_assert( kChannels * static_cast< int >( CC_COUNT ) + 8 + 4 + 1 == kPresetParamCount,
               "the builder above and the preset coverage in Controls.h disagree" );

constexpr Ch withFilter( Ch c, float filter )
{
	c.filter = filter;
	return c;
}
constexpr Ch withCv( Ch c, float source, float depth )
{
	c.cv    = source;
	c.depth = depth;
	return c;
}
constexpr Ch withMarkSpace( Ch c, float m )
{
	c.markSpace = m;
	return c;
}

inline constexpr Preset kPresets[] = {
	//The defaults. Two squares into X and Y: the beam dwells at the rails and
	//crosses between them fast, so the picture is four dots and the faint lines
	//of the crossings. Electrostatic (Coil 0), so the crossings are straight.
	make( "Four Dots", kCh1, kCh2, kCh3, kCh4, kCh5, kCh6, kPatchDots, kYokeScope, 0.50f ),

	//An RC on channel 1's output, and X taken from after it. The dots become
	//horizontal lines because X now takes its time getting across, and the
	//lines are brightest at their ends because that is where it is slowest.
	make( "Lines", withFilter( kCh1, 0.45f ), kCh2, kCh3, kCh4, kCh5, kCh6,
	      Patch{ kFilt1, kOut2, kOff, 0.55f, 0.5f, 0.5f, 0.5f, 0.0f }, kYokeScope, 0.60f ),

	//The capacitors instead of the outputs: exponentials between Vcc/3 and
	//2Vcc/3 on both axes, so every edge of the figure is a curve rather than a
	//straight run between two rails. Channel 2 is given twice channel 1's
	//period (the same decade at x2.01) so the figure closes as a two-lobed
	//curve and the curvature is visible -- at the default 1% detune the two
	//axes are so nearly in step that the figure collapses to a thin precessing
	//loop, which is the crawl demonstrated twice rather than the curvature
	//demonstrated once. The 0.01 is left in so it still drifts.
	//
	//The gain is up because a capacitor swings a third of what an output does,
	//and the brightness with it: a beam that never parks spreads the same
	//energy per frame over a far longer path.
	make( "Curves", kCh1, stock( kR10k, kR4k7, k100nF, 0.30320f ), kCh3, kCh4, kCh5, kCh6,
	      Patch{ kCap1, kCap2, kOff, 0.85f, 0.85f, 0.5f, 0.5f, 0.0f }, kYokeScope, 0.86f ),

	//Channel 2 at three halves of channel 1 -- 66.8 nF against 100 nF -- so the
	//figure is a 3:2 Lissajous, and a few hertz off exact so it crawls. A
	//little coil on both axes rounds the exponentials' corners.
	make( "Lissajous Crawl", kCh1, stock( kR10k, kR4k7, k10nF, 0.8250f ), kCh3, kCh4, kCh5, kCh6,
	      Patch{ kCap1, kCap2, kOff, 0.85f, 0.85f, 0.5f, 0.5f, 0.0f }, Yoke{ 0.3f, 0.3f, 0.5f, 0.66f }, 0.78f ),

	//Channel 4's capacitor -- the 7 Hz LFO -- into channel 1's pin 5. The
	//threshold moves, so channel 1's frequency and duty sweep with it.
	make( "FM", withCv( withFilter( kCh1, 0.35f ), kCvCh4Cap, 0.7f ), kCh2, kCh3, kCh4, kCh5, kCh6,
	      Patch{ kFilt1, kCap2, kOff, 0.55f, 0.85f, 0.5f, 0.5f, 0.0f }, Yoke{ 0.2f, 0.2f, 0.5f, 0.66f }, 0.74f ),

	//The mark/space pot pulled off centre on channel 1 and the 2 Hz LFO into
	//its pin 5. The dots' dwell -- and, through the coupling capacitor, their
	//position -- follows the duty.
	make( "Duty Sweep", withCv( withMarkSpace( kCh1, 0.75f ), kCvCh6Cap, 0.8f ), kCh2, kCh3, kCh4, kCh5, kCh6,
	      Patch{ kOut1, kCap2, kOff, 0.5f, 0.85f, 0.5f, 0.5f, 0.0f }, Yoke{ 0.25f, 0.25f, 0.5f, 0.66f }, 0.62f ),

	//Two ramps: the pot at 95% makes each capacitor a sawtooth. Channel 1 at
	//744 Hz is the line, channel 2 at 25 Hz is the frame, and Z takes channel
	//1's output with Blank When Low, so the beam is cut during the line
	//retrace -- which is the 5% of the cycle the output is low. The frame
	//retrace is not blanked; it is one fast diagonal, dim because it is fast.
	make( "Raster", withMarkSpace( kCh1, 0.95f ), withMarkSpace( stock( kR100k, kR47k, k100nF, 0.4728f ), 0.95f ),
	      kCh3, kCh4, kCh5, kCh6,
	      Patch{ kCap1, kCap2, kOut1, 0.85f, 0.85f, 0.5f, 0.5f, 1.0f }, Yoke{ 0.15f, 0.15f, 0.5f, 0.66f }, 0.80f ),
};

inline constexpr int kCount = static_cast< int >( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace astable
