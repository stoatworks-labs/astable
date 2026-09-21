#include "Controls.h"

#include "render/Phosphor.h"

#include <algorithm>
#include <cmath>

namespace astable
{

const char* const kDecadeNames[ kDecadeCount ] = { "1 nF", "10 nF", "100 nF", "1 uF", "10 uF", "100 uF" };
const double kDecadeFarads[ kDecadeCount ]     = { 1e-9, 10e-9, 100e-9, 1e-6, 10e-6, 100e-6 };

const char* const kCvSourceNames[ kCvSourceCount ] = {
	"None",
	"Ch1 Output", "Ch2 Output", "Ch3 Output", "Ch4 Output", "Ch5 Output", "Ch6 Output",
	"Ch1 Cap", "Ch2 Cap", "Ch3 Cap", "Ch4 Cap", "Ch5 Cap", "Ch6 Cap",
	"Audio",
};

const char* const kPatchSourceNames[ kPatchSourceCount ] = {
	"Off",
	"Ch1 Output", "Ch2 Output", "Ch3 Output", "Ch4 Output", "Ch5 Output", "Ch6 Output",
	"Ch1 Cap", "Ch2 Cap", "Ch3 Cap", "Ch4 Cap", "Ch5 Cap", "Ch6 Cap",
	"Ch1 Filtered", "Ch2 Filtered", "Ch3 Filtered", "Ch4 Filtered", "Ch5 Filtered", "Ch6 Filtered",
};

const char* const kZModeNames[ kZModeCount ] = { "Off", "Blank When Low", "Brightness" };

//In the order of the table in render/Phosphor.cpp. The element value is the
//table index, so this list may only ever grow at the end.
const char* const kPhosphorNames[ kPhosphorCount ] = {
	"P4 White", "P22 Colour", "P31 Green", "P1 Green", "P2 Long Green", "P7 Blue/Amber", "P11 Blue", "P39 Very Long"
};

static_assert( kPhosphorCount == 8, "kPhosphorNames must match the table in render/Phosphor.cpp" );

int Option( float value, int count )
{
	//An option parameter holds its element *value*, not a 0..1 fraction, so it
	//is rounded rather than scaled. Getting this backwards gives a dropdown
	//permanently stuck on its first entry.
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

float Exponential( float value, float low, float high )
{
	const float t = std::clamp( value, 0.0f, 1.0f );
	return low * std::pow( high / low, t );
}

float Linear( float value, float low, float high )
{
	return low + ( high - low ) * std::clamp( value, 0.0f, 1.0f );
}

double ResistanceOf( float value )
{
	return static_cast< double >( Exponential( value, 1.0e3f, 1.0e6f ) );
}

double CapacitanceOf( float decade, float fine )
{
	//A decade and a multiplier, so a value can be read off like a real part:
	//"100 nF, times 1.03" is a 103 nF capacitor, which is a 100 nF one at 3%.
	const double base = kDecadeFarads[ Option( decade, kDecadeCount ) ];
	return base * static_cast< double >( Exponential( fine, 1.0f, 10.0f ) );
}

double NominalPeriod( double ra, double rb, double c )
{
	return 0.6931471805599453 * ( ra + 2.0 * rb ) * c;
}

namespace
{
inline bool boolean( float v )
{
	return v > 0.5f;
}

/// A control whose bottom end is a true zero and whose body is exponential:
/// the exponential alone bottoms out at `low`, never at nothing, so the first
/// 2% of the travel fades it linearly to zero.
inline float ExponentialWithOff( float value, float low, float high )
{
	constexpr float kOffRamp = 0.02f;
	const float t = std::clamp( value, 0.0f, 1.0f );
	if( t <= 0.0f )
		return 0.0f;
	return Exponential( ( t - kOffRamp ) / ( 1.0f - kOffRamp ), low, high ) * std::min( 1.0f, t / kOffRamp );
}

/// The duty the Mark-Space pot asks for, with the stock duty at its centre.
///
/// Piecewise linear from 5% at one end, through the stock circuit's own duty
/// at 0.5, to 95% at the other. Piecewise because the centre has to be the
/// stock circuit exactly: the `--period` assertion is about the stock circuit
/// and it is made at 0.5.
inline double DutyFor( float markSpace, double stockDuty )
{
	const double m = std::clamp( static_cast< double >( markSpace ), 0.0, 1.0 );
	if( m < 0.5 )
		return 0.05 + ( stockDuty - 0.05 ) * ( m / 0.5 );
	return stockDuty + ( 0.95 - stockDuty ) * ( ( m - 0.5 ) / 0.5 );
}
} // namespace

Resolved Resolve( const float* p, float audioLevel )
{
	Resolved r;

	const double vcc = static_cast< double >( Linear( p[ PT_VCC ], 5.0f, 15.0f ) );

	for( int i = 0; i < kChannels; ++i )
	{
		ChannelSpec& ch = r.bench.channel[ i ];
		auto at         = [ & ]( ChannelControl c ) { return p[ ChannelParam( i, c ) ]; };

		const double ra = ResistanceOf( at( CC_RA ) );
		const double rb = ResistanceOf( at( CC_RB ) );
		const double c  = CapacitanceOf( at( CC_C_DECADE ), at( CC_C_FINE ) );

		ch.c             = c;
		ch.vcc           = vcc;
		ch.nominalPeriod = NominalPeriod( ra, rb, c );

		//----------------------------------------------------------------------
		// The Maddi mark/space trick.
		//
		// The stock circuit charges through Ra + Rb and discharges through Rb,
		// so its duty is (Ra + Rb) / (Ra + 2Rb) and cannot go below a half.
		// The trick from the comments puts a pot across the timing path with a
		// diode steering the charge through one side of the wiper and the
		// discharge through the other, so the two halves of the cycle share one
		// total resistance and the wiper only decides how it is split: the
		// period stays put and the duty goes wherever the wiper is.
		//
		// Modelled as exactly that. The total is the stock circuit's Ra + 2Rb,
		// so at the pot's centre the two resistances ARE Ra + Rb and Rb and the
		// stock formula holds to the digit; away from centre the total is kept
		// and re-split by the duty the pot asks for.
		//----------------------------------------------------------------------
		const double total = ra + 2.0 * rb;
		const double stock = ( ra + rb ) / total;
		const double duty  = DutyFor( at( CC_MARK_SPACE ), stock );
		ch.rCharge         = std::max( total * duty, 1.0 );
		ch.rDischarge      = std::max( total * ( 1.0 - duty ), 1.0 );

		ch.cvSource = Option( at( CC_CV_SOURCE ), kCvSourceCount );
		ch.cvDepth  = std::clamp( at( CC_CV_DEPTH ), 0.0f, 1.0f );
		ch.reset    = boolean( at( CC_RESET ) );

		//The filter is measured in the channel's own periods -- 0.02 to 4 of
		//them -- rather than in seconds, so it means the same thing on a 2 Hz
		//timer as on a 2 kHz one. In seconds it would be a control whose useful
		//range moved five decades depending on which channel it was on.
		ch.filterTau = static_cast< double >( ExponentialWithOff( at( CC_FILTER ), 0.02f, 4.0f ) ) * ch.nominalPeriod;
		ch.level     = std::clamp( at( CC_LEVEL ), 0.0f, 1.0f );
	}

	// -- Patch ---------------------------------------------------------------
	PatchSpec& patch = r.bench.patch;
	patch.xSource    = Option( p[ PT_X_SOURCE ], kPatchSourceCount );
	patch.ySource    = Option( p[ PT_Y_SOURCE ], kPatchSourceCount );
	patch.zSource    = Option( p[ PT_Z_SOURCE ], kPatchSourceCount );
	patch.xGain      = ExponentialWithOff( p[ PT_X_GAIN ], 0.1f, 4.0f );
	patch.yGain      = ExponentialWithOff( p[ PT_Y_GAIN ], 0.1f, 4.0f );
	patch.xOffset    = Linear( p[ PT_X_OFFSET ], -1.0f, 1.0f );
	patch.yOffset    = Linear( p[ PT_Y_OFFSET ], -1.0f, 1.0f );
	patch.zMode      = Option( p[ PT_Z_MODE ], kZModeCount );

	// -- Yoke ----------------------------------------------------------------
	YokeSpec& yoke      = r.bench.yoke;
	yoke.coilX          = static_cast< double >( ExponentialWithOff( p[ PT_COIL_X ], 10e-6f, 100e-3f ) );
	yoke.coilY          = static_cast< double >( ExponentialWithOff( p[ PT_COIL_Y ], 10e-6f, 100e-3f ) );
	yoke.deflectionGain = Linear( p[ PT_DEFLECTION_GAIN ], 0.0f, 2.0f );
	yoke.rail           = Exponential( p[ PT_AMP_RAIL ], 0.2f, 3.0f );

	// -- Supply --------------------------------------------------------------
	r.bench.audio = std::clamp( audioLevel * Linear( p[ PT_AUDIO_GAIN ], 0.0f, 4.0f ), 0.0f, 1.0f );

	// -- Tube ----------------------------------------------------------------
	TubeSettings& tube   = r.tube;
	tube.phosphor        = Option( p[ PT_PHOSPHOR ], kPhosphorCount );
	tube.persistence     = persistenceMultiplier( p[ PT_PERSISTENCE ] );
	tube.spotSigma       = Exponential( p[ PT_FOCUS ], 0.0012f, 0.02f );
	tube.faceAspect      = Exponential( p[ PT_FACE_ASPECT ], 1.0f, 2.0f );
	tube.cornerRadius    = std::clamp( p[ PT_CORNER_RADIUS ], 0.0f, 1.0f );
	tube.deflectionGain  = Linear( p[ PT_OVERSCAN ], 0.6f, 1.4f );
	//Exponential around a calibrated 1.0, with a true off at the bottom of the
	//travel: vectrix's Beam control, for vectrix's reasons.
	tube.beamPower       = std::pow( 10.0f, -1.0f + 2.0f * std::clamp( p[ PT_BRIGHTNESS ], 0.0f, 1.0f ) )
	                       * std::min( 1.0f, p[ PT_BRIGHTNESS ] * 50.0f );

	return r;
}

} // namespace astable
