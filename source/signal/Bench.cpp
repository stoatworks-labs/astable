#include "signal/Bench.h"

#include <algorithm>
#include <cmath>

namespace astable
{
namespace
{
inline double relax( double value, double target, double dt, double tau )
{
	if( !( tau > 0.0 ) )
		return target;
	return target + ( value - target ) * std::exp( -dt / tau );
}
} // namespace

void Bench::Prepare()
{
	block.assign( kMaxBlock, Sample{} );
	Reset();
}

void Bench::Reset()
{
	for( Timer555& t : timers )
		t.Reset();
	meanX = meanY = 0.0;
	coilX = coilY = 0.0;
}

void Bench::SetParams( const BenchParams& wanted )
{
	params = wanted;

	double fastest = 0.0;
	for( int i = 0; i < kChannels; ++i )
	{
		const ChannelSpec& ch = params.channel[ i ];

		Timer555::Params tp;
		tp.rCharge    = ch.rCharge;
		tp.rDischarge = ch.rDischarge;
		tp.c          = ch.c;
		tp.vcc        = ch.vcc;
		tp.reset      = ch.reset;
		tp.filterTau  = ch.filterTau;
		timers[ i ].SetParams( tp );

		//Only a running channel needs resolving. One held in reset is a flat
		//line at whatever rate it is sampled.
		if( ch.reset && ch.nominalPeriod > 0.0 )
			fastest = std::max( fastest, 1.0 / ch.nominalPeriod );
	}

	rate = std::clamp( kSamplesPerPeriod * fastest, kMinRate, kMaxRate );
}

double Bench::CvSignal( int source ) const
{
	if( source <= 0 )
		return 0.0;
	if( source <= kChannels )
	{
		//An output: -1 low, +1 high, in terms of the supply. A bipolar part's
		//high is short of the rail, so this never quite reaches +1 -- which is
		//what the pin 5 of the next timer actually sees.
		const Timer555& t = timers[ source - 1 ];
		return std::clamp( ( t.Out() - 0.5 * t.OutHigh() - 0.5 * t.OutLow() ) / ( 0.5 * ( t.OutHigh() - t.OutLow() ) ), -1.0, 1.0 );
	}
	if( source <= 2 * kChannels )
	{
		//A capacitor: Vcc/3 .. 2Vcc/3 spans -1..+1, so a stock timer's cap
		//modulates by its whole swing rather than by a third of it.
		const int channel = source - 1 - kChannels;
		const double vcc  = params.channel[ channel ].vcc;
		return std::clamp( ( timers[ channel ].Cap() - 0.5 * vcc ) / ( vcc / 6.0 ), -1.0, 1.0 );
	}
	//Audio: 0..1, pushing the pin UP from rest, so silence is the stock circuit
	//and a signal lowers the frequency. Bipolar would make silence the extreme.
	return std::clamp( static_cast< double >( params.audio ), 0.0, 1.0 );
}

double Bench::ControlVoltage( int channel ) const
{
	const ChannelSpec& ch = params.channel[ channel ];
	const double rest     = ch.vcc * ( 2.0 / 3.0 );
	if( ch.cvSource <= 0 || !( ch.cvDepth > 0.0f ) )
		return rest;

	//Pin 5 sits at 2/3 Vcc through the internal divider. Driving it through a
	//resistor pulls it toward the source; depth is that resistor. The 0.45 is
	//how far a full-depth source can move it -- to 0.37 Vcc or 0.97 Vcc -- and
	//the timer's own clamp keeps the top end oscillating.
	return rest * ( 1.0 + 0.45 * static_cast< double >( ch.cvDepth ) * CvSignal( ch.cvSource ) );
}

double Bench::PatchSignal( int source ) const
{
	if( source <= 0 )
		return 0.0;

	const int kind    = ( source - 1 ) / kChannels;   //0 output, 1 cap, 2 filtered
	const int channel = ( source - 1 ) % kChannels;
	const Timer555& t = timers[ channel ];
	const ChannelSpec& ch = params.channel[ channel ];

	double v;
	switch( kind )
	{
		case 0: v = t.Out(); break;
		case 1: v = t.Cap(); break;
		default: v = t.Filtered(); break;
	}

	//Deflection volts: Vcc is +1, ground is -1. The same scale for every kind
	//of signal, so a capacitor (a third of the swing) really is a third the
	//size of an output on the screen, and Gain is what makes it bigger.
	return ( v - 0.5 * ch.vcc ) / ( 0.5 * ch.vcc ) * static_cast< double >( ch.level );
}

const Sample* Bench::Render( int n, double frameSeconds )
{
	n = std::clamp( n, 2, kMaxBlock );
	const double dt = frameSeconds / static_cast< double >( n - 1 );

	const PatchSpec& patch = params.patch;
	const YokeSpec& yoke   = params.yoke;

	auto zOf = [ & ]() -> float {
		if( patch.zMode == 0 || patch.zSource <= 0 )
			return 1.0f;
		//Z reads the raw signal, not the coupled one: blanking on "pin 3 low"
		//has to mean pin 3, not the yoke.
		const double s = PatchSignal( patch.zSource );
		if( patch.zMode == 1 )
			return s > 0.0 ? 1.0f : 0.0f;
		return static_cast< float >( std::clamp( 0.5 * ( s + 1.0 ), 0.0, 1.0 ) );
	};

	auto drive = [ & ]( int source, float gain, float offset, double& mean, double& coil, double coilTau, double stepDt ) -> float {
		double d = PatchSignal( source ) * static_cast< double >( gain ) * static_cast< double >( yoke.deflectionGain );

		//The coupling capacitor: subtract the running mean. Then the centring
		//pot, then the amplifier runs out of headroom, then the coil follows.
		if( stepDt > 0.0 )
			mean = relax( mean, d, stepDt, kCouplingTau );
		d -= mean;
		d += static_cast< double >( offset );

		const double rail = std::max( static_cast< double >( yoke.rail ), 1e-3 );
		d = std::clamp( d, -rail, rail );

		if( stepDt > 0.0 )
			coil = relax( coil, d, stepDt, coilTau );
		else if( !( coilTau > 0.0 ) )
			coil = d;
		return static_cast< float >( coil );
	};

	for( int i = 0; i < n; ++i )
	{
		//Sample 0 is the state the last block left, un-stepped. See Bench.h.
		const double stepDt = i > 0 ? dt : 0.0;
		if( i > 0 )
		{
			//Every channel's pin 5 is read before any channel steps, so the
			//order of the six on the breadboard does not matter -- a timer
			//modulating the one before it sees the same one-interval lag as a
			//timer modulating the one after.
			double v5[ kChannels ];
			for( int c = 0; c < kChannels; ++c )
				v5[ c ] = ControlVoltage( c );
			for( int c = 0; c < kChannels; ++c )
				timers[ c ].Step( dt, v5[ c ] );
		}

		Sample& s = block[ static_cast< size_t >( i ) ];
		s.x  = drive( patch.xSource, patch.xGain, patch.xOffset, meanX, coilX, yoke.coilX, stepDt );
		s.y  = drive( patch.ySource, patch.yGain, patch.yOffset, meanY, coilY, yoke.coilY, stepDt );
		s.z  = zOf();
		s.dt = static_cast< float >( dt );
	}

	return block.data();
}

} // namespace astable
