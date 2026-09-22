#pragma once

namespace astable
{
/**
	One 555 in astable, simulated at the component level.

	Not a waveform generator that happens to have a 555's shape: the state is a
	capacitor voltage and a flip-flop, and the output is whatever the two
	comparators make of them. That is what lets the control-voltage pin, the
	reset pin and the mark/space pot all be *inputs to the same circuit* rather
	than three special cases -- and it is why the period comes out at
	0.693 (Ra + 2Rb) C without that number appearing anywhere in the code.

	## The circuit

	    charge:    C charges through rCharge toward Vcc while the output is high
	    threshold: at V5 (pin 5; 2/3 Vcc with nothing connected) the flip-flop
	               resets, the output goes low, and pin 7 sinks
	    discharge: C discharges through rDischarge toward ground
	    trigger:   at V5/2 the flip-flop sets and it starts again

	In the stock circuit rCharge is Ra + Rb and rDischarge is Rb. The Maddi
	mark/space pot changes both while keeping their sum -- see Controls.cpp.

	## Why the crossing is solved, not sampled

	A sampled comparator finds the crossing up to a sample late. At 96 samples a
	period that is a 1% error on the period and the same again on the duty --
	every cycle, always in the same direction, so it does not average out. The
	assertion this plugin exists to pass is "period within 1%", and a
	sample-quantised comparator fails it on principle rather than by accident.

	So `Step` solves the exponential for the exact instant the capacitor reaches
	the comparator's level, switches state THERE, and integrates the remainder
	of the interval in the new state. The output pin still flips on a sample
	boundary as far as the yoke is concerned -- the drive is only sampled once
	per interval -- but the *timing* the flip-flop keeps is exact, so the phase
	never accumulates an error and two channels beat at the rate their parts
	say they should.

	Solving for a crossing is not the same as waiting for one, and pin 5 is
	where the difference shows. A control voltage moves the comparator levels
	between one interval and the next, and it can move one of them straight
	past the capacitor: the threshold pulled down under an already-charged C,
	or the trigger pushed up over an already-drained one. The crossing is then
	in the past and there is no future one to solve for -- but the comparators
	are level comparators, so the part flips immediately, and `Step` does the
	same. Miss that and the channel latches on the rail it was heading for and
	stays there for the life of the instance, with pin 4 no escape: a reset
	drains C below the trigger, which is the same trap the other way up. See
	`attest --recover`.

	## The output stage

	A bipolar 555 swings from about 0.1 V to Vcc - 1.7 V, not rail to rail, and
	takes about 100 ns to get there. Both are modelled honestly: the levels are
	real, and the edge is a single-pole slew with a 45 ns time constant -- which
	at any rate this engine runs at is one sample or less, i.e. effectively
	instant. The rounding a viewer sees on the edges comes from the yoke, where
	it comes from on a television.
*/
class Timer555
{
public:
	struct Params
	{
		double rCharge    = 14700.0; ///< Ohms, output high.
		double rDischarge = 4700.0;  ///< Ohms, output low.
		double c          = 100e-9;  ///< Farads.
		double vcc        = 9.0;     ///< Volts. Sets the output swing; the timing is independent of it.
		bool reset        = true;    ///< Pin 4. False holds the output low and discharges C.
		double filterTau  = 0.0;     ///< Seconds. An RC on pin 3; 0 is a wire.
	};

	void SetParams( const Params& params );

	/// Back to the moment of power-up: C empty, output high, nothing in the
	/// filter. Also zeroes the measurements below.
	void Reset();

	/// Advance one interval. `vControl` is the voltage on pin 5, which is the
	/// threshold; the trigger is half of it.
	void Step( double dt, double vControl );

	double Cap() const
	{
		return vc;
	}
	double Out() const
	{
		return vout;
	}
	double Filtered() const
	{
		return vfilt;
	}
	bool High() const
	{
		return high;
	}

	/// Pin 3's two levels, for whoever normalises the output.
	double OutHigh() const
	{
		return outHigh;
	}
	double OutLow() const
	{
		return outLow;
	}

	/// The last complete cycle, from the exact crossing times. Zero until two
	/// rising edges have been seen. For the harness; nothing in the picture
	/// reads them.
	double LastPeriod() const
	{
		return lastPeriod;
	}
	double LastHighTime() const
	{
		return lastHighTime;
	}

private:
	Params p;

	double vc     = 0.0;
	double vout   = 0.0;
	double vfilt  = 0.0;
	bool high     = true;

	double outHigh = 7.3;
	double outLow  = 0.1;

	double now          = 0.0;
	double lastRise     = -1.0;
	double lastFall     = -1.0;
	double lastPeriod   = 0.0;
	double lastHighTime = 0.0;
};

} // namespace astable
