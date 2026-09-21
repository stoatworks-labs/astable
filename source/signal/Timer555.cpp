#include "signal/Timer555.h"

#include <algorithm>
#include <cmath>

namespace astable
{
namespace
{
/// The output stage's slew. ~100 ns rise time is a 45 ns time constant, which
/// is under one interval at every rate this engine runs at.
constexpr double kSlewTau = 45e-9;

/// What pin 7 discharges the capacitor toward. Ground, as the datasheet's
/// 0.693 assumes: a saturated transistor's tenth of a volt would put the
/// trigger crossing 1.2% early at 9 V and 2% early at 5 V, every cycle, and
/// the assertion this plugin makes is tighter than that.
constexpr double kSinkVolts = 0.0;

/// How many comparator crossings one interval is allowed to contain before
/// the loop gives up. A channel running a thousand times faster than the
/// engine rate can flip that often per interval; the picture it makes is a
/// smear whatever happens, and the bound stops it being a hang.
constexpr int kMaxFlipsPerStep = 64;

inline double relax( double value, double target, double dt, double tau )
{
	if( !( tau > 0.0 ) )
		return target;
	return target + ( value - target ) * std::exp( -dt / tau );
}
} // namespace

void Timer555::SetParams( const Params& params )
{
	p = params;
	p.rCharge    = std::max( p.rCharge, 1.0 );
	p.rDischarge = std::max( p.rDischarge, 1.0 );
	p.c          = std::max( p.c, 1e-13 );
	p.vcc        = std::max( p.vcc, 1.0 );
	p.filterTau  = std::max( p.filterTau, 0.0 );

	//A bipolar 555's output transistor drops about 1.7 V high and sits about
	//0.1 V low. A CMOS part would go rail to rail; this is the part on the
	//breadboard in the video.
	outHigh = std::max( p.vcc - 1.7, 0.5 );
	outLow  = 0.1;
}

void Timer555::Reset()
{
	vc    = 0.0;
	vout  = outLow;
	vfilt = outLow;
	high  = true;

	now          = 0.0;
	lastRise     = -1.0;
	lastFall     = -1.0;
	lastPeriod   = 0.0;
	lastHighTime = 0.0;
}

void Timer555::Step( double dt, double vControl )
{
	if( !( dt > 0.0 ) )
		return;

	//The comparators' levels. The clamp keeps a driven pin 5 inside the range
	//where the part still oscillates: a threshold at or above Vcc is one the
	//capacitor can never reach, and a real 555 driven that hard simply stalls
	//high -- which is also what this does, at the top of the clamp, only
	//slowly.
	const double vth   = std::clamp( vControl, 0.10 * p.vcc, 0.95 * p.vcc );
	const double vtrig = 0.5 * vth;

	double remaining = dt;

	if( !p.reset )
	{
		//Pin 4 low: the flip-flop is held reset, pin 7 sinks, and the capacitor
		//drains through Rb. The trigger comparator cannot set it again until
		//reset is released.
		high = false;
		vc   = relax( vc, kSinkVolts, remaining, p.rDischarge * p.c );
		remaining = 0.0;
	}

	for( int flips = 0; remaining > 0.0 && flips < kMaxFlipsPerStep; ++flips )
	{
		const double target = high ? p.vcc : kSinkVolts;
		const double tau    = ( high ? p.rCharge : p.rDischarge ) * p.c;
		const double level  = high ? vth : vtrig;

		//Time to reach the comparator from here, on this exponential. Solved in
		//closed form: v(t) = target + (vc - target) e^{-t/tau}, so the level is
		//crossed at t = tau ln((vc - target) / (level - target)). The ratio is
		//positive whenever the level lies between vc and the target, which is
		//the only case in which the crossing exists at all.
		const double num   = vc - target;
		const double den   = level - target;
		const bool reaches = ( high ? ( vc < level && level < target ) : ( vc > level && level > target ) )
		                     && num / den > 1.0;
		const double tCross = reaches ? tau * std::log( num / den ) : -1.0;

		if( !reaches || tCross >= remaining )
		{
			vc = relax( vc, target, remaining, tau );
			now += remaining;
			remaining = 0.0;
			break;
		}

		//Land exactly on the level, at the exact time, and flip.
		vc = level;
		now += tCross;
		remaining -= tCross;

		if( high )
		{
			//Threshold reached: output falls, discharge begins.
			high = false;
			if( lastRise >= 0.0 )
				lastHighTime = now - lastRise;
			lastFall = now;
		}
		else
		{
			//Trigger reached: output rises, charge begins.
			high = true;
			if( lastRise >= 0.0 )
				lastPeriod = now - lastRise;
			lastRise = now;
		}
	}

	if( remaining > 0.0 )
	{
		//Hit the flip bound. Whatever is left of the interval is spent at the
		//comparator level the loop stopped on; the timing is wrong for this one
		//interval and right again from the next.
		now += remaining;
	}

	//The output stage and the RC after it, once per interval on the state the
	//flip-flop ended the interval in.
	vout  = relax( vout, high ? outHigh : outLow, dt, kSlewTau );
	vfilt = p.filterTau > 0.0 ? relax( vfilt, vout, dt, p.filterTau ) : vout;
}

} // namespace astable
