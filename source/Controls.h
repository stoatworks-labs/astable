#pragma once

#include "signal/Bench.h"

namespace astable
{
/**
	Parameter ids.

	**The declaration order in Astable.cpp is the order the host shows them, and
	`SetParamGroup` collapses *runs* of consecutive ids into one group.** So
	reordering this enum does not merely rearrange the panel -- it silently
	splits a group in two, or merges two into one, and the result renders as a
	duplicated group header that reads as a bug.

	**Renaming a released parameter is not safe; renumbering is.** A saved
	composition stores name/value pairs (measured on Arena 7.27.1, see vectrix's
	notes), so a renamed control silently loses its saved value. The append-only
	rule holds for an OPTION's ELEMENTS, which are stored as numbers.

	## The six channels are one run

	Channel n's ten controls sit at `PT_CHANNEL_FIRST + n * CC_COUNT + control`,
	so the whole bench -- six channels, the patch bay and the yoke -- is one
	contiguous run from `PT_CHANNEL_FIRST` to `PT_AMP_RAIL`. That run IS the
	preset row (`Presets.h`): a preset covers exactly it and nothing else. The
	Supply group therefore sits after Yoke rather than before it as the spec
	listed, because `Audio` is a buffer parameter that cannot live inside a
	preset row and Vcc is the bench supply rather than the patch.
*/
enum ChannelControl : unsigned int
{
	CC_RA = 0,     ///< 1 kR .. 1 MR, log
	CC_RB,         ///< 1 kR .. 1 MR, log
	CC_C_DECADE,   ///< 1 nF .. 100 uF, an option
	CC_C_FINE,     ///< x1 .. x10 on the decade, log
	CC_CV_SOURCE,  ///< what drives pin 5
	CC_CV_DEPTH,
	CC_MARK_SPACE, ///< the pot-and-diode trick; 0.5 is the stock circuit
	CC_RESET,      ///< pin 4. Off holds pin 3 low and discharges C.
	CC_FILTER,     ///< an RC on pin 3, in periods
	CC_LEVEL,      ///< an attenuator on everything the channel feeds out
	CC_COUNT
};

enum ParamId : unsigned int
{
	PT_CHANNEL_FIRST = 0,
	PT_CHANNEL_LAST  = PT_CHANNEL_FIRST + kChannels * CC_COUNT - 1,

	// -- Patch bay -----------------------------------------------------------
	PT_X_SOURCE,
	PT_Y_SOURCE,
	PT_Z_SOURCE,
	PT_X_GAIN,
	PT_Y_GAIN,
	PT_X_OFFSET,
	PT_Y_OFFSET,
	PT_Z_MODE,

	// -- Yoke ----------------------------------------------------------------
	PT_COIL_X,
	PT_COIL_Y,
	PT_DEFLECTION_GAIN,
	PT_AMP_RAIL,

	// -- Supply --------------------------------------------------------------
	PT_VCC,
	PT_AUDIO_FFT,
	PT_AUDIO_GAIN,

	// -- Tube ----------------------------------------------------------------
	PT_PHOSPHOR,
	PT_PERSISTENCE,
	PT_FOCUS,
	PT_FACE_ASPECT,
	PT_CORNER_RADIUS,
	PT_OVERSCAN,
	PT_BRIGHTNESS,

	// -- Preset --------------------------------------------------------------
	PT_PRESET,

	// -- The Stoatworks About block ------------------------------------------
	//
	// One display-only text line followed by one button per link the branding
	// header carries. Astable.cpp static_asserts this run against
	// `about::kParamCount`; a guide URL added later would otherwise shift
	// PT_COUNT and leave the last button undeclared.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,

	PT_COUNT
};

/// The id of one channel's control.
constexpr unsigned int ChannelParam( int channel, ChannelControl control )
{
	return PT_CHANNEL_FIRST + static_cast< unsigned int >( channel ) * CC_COUNT + control;
}

//---------------------------------------------------------------------------
// What a preset covers: the whole bench, plus Brightness.
//
// The bench -- six channels, the patch bay, the yoke -- is one contiguous id
// run, so it is a subtraction. Brightness is not adjacent to it and is covered
// anyway, for a reason that is the plugin's own physics rather than a
// convenience: the energy a frame deposits is fixed, so a figure that parks
// the beam at four corners and one that sweeps it along a continuous curve
// differ in peak brightness by two orders of magnitude. Four Dots is well
// exposed at the default and Curves is very nearly black. A preset that ships
// unreadable is not a preset, and the alternative -- making the operator hunt
// for the Brightness control after every change -- is exactly what turning the
// brightness knob on a television is, which is to say it belongs to the
// machine rather than to the room.
//
// The supply and the rest of the Tube group stay out: which television this is
// on is a different question from which breadboard you are watching.
//---------------------------------------------------------------------------
constexpr unsigned int kPresetFirst = PT_CHANNEL_FIRST;
constexpr int kBenchColumns         = static_cast< int >( PT_AMP_RAIL - PT_CHANNEL_FIRST + 1 );
constexpr int kPresetParamCount     = kBenchColumns + 1;

/// Which column of a preset row drives this parameter, or -1 for one no preset
/// covers.
constexpr int PresetColumn( unsigned int id )
{
	if( id >= kPresetFirst && id <= PT_AMP_RAIL )
		return static_cast< int >( id - kPresetFirst );
	if( id == PT_BRIGHTNESS )
		return kBenchColumns;
	return -1;
}

/// The parameter a column drives: the inverse of the above.
constexpr unsigned int PresetParamId( int column )
{
	return column < kBenchColumns ? kPresetFirst + static_cast< unsigned int >( column ) : PT_BRIGHTNESS;
}

/// Option-parameter element counts, so the declaration and the reader cannot
/// disagree about how many entries a dropdown has.
constexpr int kDecadeCount      = 6;
constexpr int kCvSourceCount    = 2 + 2 * kChannels; ///< None, six outputs, six caps, Audio
constexpr int kPatchSourceCount = 1 + 3 * kChannels; ///< Off, six outputs, six caps, six filtered
constexpr int kZModeCount       = 3;
constexpr int kPhosphorCount    = 8;

extern const char* const kDecadeNames[ kDecadeCount ];
extern const double kDecadeFarads[ kDecadeCount ];
extern const char* const kCvSourceNames[ kCvSourceCount ];
extern const char* const kPatchSourceNames[ kPatchSourceCount ];
extern const char* const kZModeNames[ kZModeCount ];
extern const char* const kPhosphorNames[ kPhosphorCount ];

/// The number of FFT bins the host is asked for: the fleet's figure.
constexpr int kAudioBins = 64;

//---------------------------------------------------------------------------
// 0..1 to engineering units.
//
// Every continuous host parameter is 0..1 and mapped here. That is not a
// stylistic choice: `CFFGLPluginManager::SetParamInfo` clamps a default into
// 0..1 *before* returning, and `SetParamRange` can only be called afterwards
// because it looks the parameter up by id. So a STANDARD parameter declared in
// ohms cannot declare a default in ohms -- 10000 silently becomes 1.
//---------------------------------------------------------------------------

/// Read an option parameter's element value as an index.
int Option( float value, int count );

/// Exponential map, for anything measured in ohms, farads or seconds where the
/// useful range spans decades.
float Exponential( float value, float low, float high );

/// Linear map.
float Linear( float value, float low, float high );

/// Ra and Rb in ohms, C in farads, from the four host controls.
double ResistanceOf( float value );
double CapacitanceOf( float decade, float fine );

/// The stock period a channel would have with the pot centred: ln2 (Ra + 2Rb) C.
/// Also what the Output Filter's time constant is measured in.
double NominalPeriod( double ra, double rb, double c );

/// What the renderer needs that is not the bench: everything in the Tube group.
struct TubeSettings
{
	int phosphor        = 0;
	float persistence   = 1.0f; ///< multiplier on tau
	float spotSigma     = 0.004f;
	float faceAspect    = 4.0f / 3.0f;
	float cornerRadius  = 0.18f;
	float deflectionGain = 1.05f;///< Overscan: beam units per volt
	float beamPower     = 1.0f;
};

/// Everything the engine and the renderer run on this frame.
struct Resolved
{
	BenchParams bench;
	TubeSettings tube;
};

/// Assemble every block's parameters from the raw 0..1 array. `audioLevel` is
/// the smoothed 0..1 level the plugin folded out of the host's spectrum.
Resolved Resolve( const float* params, float audioLevel );

} // namespace astable
