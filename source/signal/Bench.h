#pragma once

#include "signal/Signal.h"
#include "signal/Timer555.h"

#include <vector>

namespace astable
{
constexpr int kChannels = 6;

/// What one channel is set to, in engineering units. `Controls.cpp` makes these
/// from the host's 0..1 values; the harness makes them directly, which is how
/// it can ask for an exact 4.7 kR rather than a slider position.
struct ChannelSpec
{
	double rCharge    = 14700.0;
	double rDischarge = 4700.0;
	double c          = 100e-9;
	double vcc        = 9.0;
	int cvSource      = 0;    ///< 0 none, 1..6 channel outputs, 7..12 channel caps, 13 audio
	float cvDepth     = 0.0f; ///< 0..1
	bool reset        = true;
	double filterTau  = 0.0;  ///< seconds
	float level       = 1.0f; ///< 0..1, an attenuator on everything the channel feeds out

	/// ln2 (Ra + 2Rb) C at the pot's centre. What the engine rate is chosen
	/// from; nothing in the simulation reads it.
	double nominalPeriod = 1.34e-3;
};

/// What the patch bay is plugged into. Sources: 0 off, 1..6 outputs, 7..12
/// caps, 13..18 filtered outputs.
struct PatchSpec
{
	int xSource   = 1;
	int ySource   = 2;
	int zSource   = 0;
	float xGain   = 0.63f;
	float yGain   = 0.63f;
	float xOffset = 0.0f; ///< deflection volts
	float yOffset = 0.0f;
	int zMode     = 0;    ///< 0 off, 1 blank when low, 2 brightness follows
};

struct YokeSpec
{
	double coilX         = 0.0; ///< L/R, seconds. 0 is an electrostatic scope.
	double coilY         = 0.0;
	float deflectionGain = 1.0f;
	float rail           = 1.2f; ///< Deflection volts the amplifier clips at.
};

struct BenchParams
{
	ChannelSpec channel[ kChannels ];
	PatchSpec patch;
	YokeSpec yoke;
	float audio = 0.0f; ///< 0..1, already through Audio Gain
};

/**
	Six timers, a patch bay and a yoke: the breadboard.

	Turns one video frame into a block of beam positions. Nothing in here
	touches GL, so the whole signal path is testable offline and, more
	usefully, deterministic: `Render` is handed a sample count and a frame
	length and reads no clock of its own.

	## The engine rate is chosen from the parts

	A 555 running at 1 Hz and one running at 50 kHz cannot share a sample rate
	that suits both, so the rate is picked per frame from the fastest running
	channel: `kSamplesPerPeriod` samples of its period, clamped to
	[kMinRate, kMaxRate]. Above the cap the exponential is under-resolved -- the
	picture goes smooth where it should have corners -- but the timing does not
	drift, because `Timer555` solves its crossings rather than sampling them.
	The cap exists because the block is drawn as one instanced quad per
	sample; 384 kHz at 60 fps is 6400 quads a frame, which is comfortable, and
	a 480 kHz timer (1 kR, 1 kR, 1 nF) would want sixty times that.

	Changing the rate between frames costs nothing: the timers' state is
	voltage, not a sample index, and `dt` travels in every sample.

	## The block covers the frame exactly

	Sample 0 is the state the last block ended in, un-stepped; samples 1..n-1
	each follow one step of `frameSeconds / (n - 1)`. So the n-1 intervals the
	renderer draws add up to exactly one frame of engine time, the energy they
	deposit is exactly `BeamPower * frameSeconds`, and there is no gap and no
	double-drawn interval at the block boundary. This is different from
	vectrix, whose sources step every sample and run 1/(n-1) fast.

	## The yoke amplifier is capacitor-coupled

	A bipolar 555 swings from 0.1 V to Vcc - 1.7 V: not symmetric about Vcc/2,
	and by an amount that changes with Vcc. Into a DC-coupled amplifier the
	four dots sit visibly off centre and slide when the supply is turned. A
	television's deflection stages are coupled through capacitors, so this one
	is too: a 2 s time constant, long enough that a 2 Hz LFO figure loses
	nothing, short enough that the picture settles in a few seconds. Offset is
	added AFTER the coupling, as the amplifier's centring pot is. One
	consequence is physical and worth knowing: a figure whose duty changes
	moves, because its mean has.
*/
class Bench
{
public:
	static constexpr double kSamplesPerPeriod = 96.0;
	static constexpr double kMinRate          = 24000.0;
	static constexpr double kMaxRate          = 384000.0;
	static constexpr double kCouplingTau      = 2.0;

	void Prepare();
	void Reset();

	/// Apply this frame's parameters. Cheap; call every frame. Does NOT clear
	/// state: turning a knob on a running bench must not restart the timers.
	void SetParams( const BenchParams& params );

	/// The rate the next Render should be asked for, from the current parameters.
	double SampleRate() const
	{
		return rate;
	}

	/// Synthesise `n` samples covering `frameSeconds` of engine time.
	const Sample* Render( int n, double frameSeconds );

	/// One timer, for the harness.
	const Timer555& Timer( int channel ) const
	{
		return timers[ channel ];
	}

	/// The modulation a CV source presents, in -1..1 -- what pin 5 is pushed by
	/// before depth is applied. Exposed so the harness can quote it.
	double CvSignal( int source ) const;

private:
	/// A patch source in deflection volts: (v - Vcc/2) / (Vcc/2), times Level.
	double PatchSignal( int source ) const;

	/// Pin 5's voltage for one channel this interval.
	double ControlVoltage( int channel ) const;

	BenchParams params;
	Timer555 timers[ kChannels ];

	double rate = kMinRate;

	//The yoke, per axis: the coupling capacitor's memory and the coil current.
	double meanX = 0.0, meanY = 0.0;
	double coilX = 0.0, coilY = 0.0;

	std::vector< Sample > block;
};

} // namespace astable
