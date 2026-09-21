#include "Astable.h"

#include "Diag.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"
#include "render/Phosphor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace astable
{
namespace
{
//The About block is a text line plus one button per link the branding header
//carries. If a user-guide URL is added later this fires, rather than the last
//button quietly going undeclared and the host showing an unnamed control.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About enum run does not match the number of About parameters" );

/// A defaults table, so that "what does this plugin do when you drop it on a
/// layer" is one readable list. The bench half of it IS preset row 1 -- see
/// Presets.h -- and `attest --defaults` holds the two together.
struct Default
{
	unsigned int id;
	float value;
};

constexpr Default kDefaults[] = {
	{ PT_VCC, 0.4f },//9 V
	{ PT_AUDIO_GAIN, 0.25f },

	//"Dixons portable": 4:3, white phosphor, a little overscan, the spot a
	//television's rather than a scope's.
	{ PT_PHOSPHOR, 0.0f },
	{ PT_PERSISTENCE, kPersistenceUnityPoint },
	{ PT_FOCUS, 0.40f },
	{ PT_FACE_ASPECT, 0.415f },//4:3 on a 1..2 log
	{ PT_CORNER_RADIUS, 0.18f },
	{ PT_OVERSCAN, 0.5625f },//1.05 on 0.6..1.4
	//Brightness is NOT here: it is a preset column, so row 1 supplies it.

	{ PT_PRESET, 0.0f },
};

/// The renderer's fixed settings: what vectrix exposes as controls and a
/// portable television does not have knobs for.
constexpr float kSpotDefocus       = 0.35f;
constexpr float kHalation          = 1.4f; //vectrix's 0.35 on its 0..4
constexpr float kHalationRadius    = 0.5f;
constexpr float kHalationThreshold = 0.04f;
constexpr float kCurvature         = 0.35f;
constexpr float kVignette          = 0.25f;
} // namespace

AstablePlugin::AstablePlugin()
{
	//A source: no input.
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	//Everything a preset covers comes from row 1, the rest from the table --
	//so "the defaults" and "preset 1" are the same thing by construction, and
	//`attest --defaults` checks that they have stayed that way.
	for( int j = 0; j < kPresetParamCount; ++j )
		params[ PresetParamId( j ) ] = presets::kPresets[ 0 ].v[ static_cast< size_t >( j ) ];
	for( const Default& d : kDefaults )
		params[ d.id ] = d.value;

	declareParameters();
	bench.Prepare();

	diag::init();
}

void AstablePlugin::declareParameters()
{
	auto standard = [ this ]( unsigned int id, const char* name ) {
		//SetParamInfo clamps a STANDARD default into 0..1, and every default in
		//params[] already is, so the value goes in as it stands.
		SetParamInfo( id, name, FF_TYPE_STANDARD, params[ id ] );
	};
	auto option = [ this ]( unsigned int id, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};
	auto boolean = [ this ]( unsigned int id, const char* name ) {
		SetParamInfo( id, name, FF_TYPE_BOOLEAN, params[ id ] > 0.5f );
	};

	// -- Channel 1..6 --------------------------------------------------------
	//
	//Names are unique across channels because `--set` and the sweep find a
	//parameter by name, and all of them fit FFGL's 16 characters: the host
	//truncates silently past that. "Ch1 Mark-Space" is fourteen.
	for( int c = 0; c < kChannels; ++c )
	{
		char buffer[ 32 ];
		auto named = [ & ]( const char* suffix ) -> const char* {
			std::snprintf( buffer, sizeof( buffer ), "Ch%d %s", c + 1, suffix );
			return buffer;
		};
		standard( ChannelParam( c, CC_RA ), named( "Ra" ) );
		standard( ChannelParam( c, CC_RB ), named( "Rb" ) );
		option( ChannelParam( c, CC_C_DECADE ), named( "C Decade" ), kDecadeNames, kDecadeCount );
		standard( ChannelParam( c, CC_C_FINE ), named( "C Fine" ) );
		option( ChannelParam( c, CC_CV_SOURCE ), named( "CV Source" ), kCvSourceNames, kCvSourceCount );
		standard( ChannelParam( c, CC_CV_DEPTH ), named( "CV Depth" ) );
		standard( ChannelParam( c, CC_MARK_SPACE ), named( "Mark-Space" ) );
		boolean( ChannelParam( c, CC_RESET ), named( "Reset" ) );
		standard( ChannelParam( c, CC_FILTER ), named( "Filter" ) );
		standard( ChannelParam( c, CC_LEVEL ), named( "Level" ) );

		std::snprintf( buffer, sizeof( buffer ), "Channel %d", c + 1 );
		SetParamGroup( ChannelParam( c, CC_RA ), buffer );
	}

	// -- Patch ---------------------------------------------------------------
	option( PT_X_SOURCE, "X Source", kPatchSourceNames, kPatchSourceCount );
	option( PT_Y_SOURCE, "Y Source", kPatchSourceNames, kPatchSourceCount );
	option( PT_Z_SOURCE, "Z Source", kPatchSourceNames, kPatchSourceCount );
	standard( PT_X_GAIN, "X Gain" );
	standard( PT_Y_GAIN, "Y Gain" );
	standard( PT_X_OFFSET, "X Offset" );
	standard( PT_Y_OFFSET, "Y Offset" );
	option( PT_Z_MODE, "Z Mode", kZModeNames, kZModeCount );
	SetParamGroup( PT_X_SOURCE, "Patch" );

	// -- Yoke ----------------------------------------------------------------
	standard( PT_COIL_X, "Coil X" );
	standard( PT_COIL_Y, "Coil Y" );
	standard( PT_DEFLECTION_GAIN, "Deflection Gain" );
	standard( PT_AMP_RAIL, "Amp Rail" );
	SetParamGroup( PT_COIL_X, "Yoke" );

	// -- Supply --------------------------------------------------------------
	standard( PT_VCC, "Vcc" );
	//The host fills this with its own FFT; the plugin never sets it.
	SetBufferParamInfo( PT_AUDIO_FFT, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO_FFT, static_cast< unsigned int >( i ), "", 0.0f );
	standard( PT_AUDIO_GAIN, "Audio Gain" );
	SetParamGroup( PT_VCC, "Supply" );

	// -- Tube ----------------------------------------------------------------
	option( PT_PHOSPHOR, "Phosphor", kPhosphorNames, kPhosphorCount );
	standard( PT_PERSISTENCE, "Persistence" );
	standard( PT_FOCUS, "Focus" );
	standard( PT_FACE_ASPECT, "Face Aspect" );
	standard( PT_CORNER_RADIUS, "Corner Radius" );
	standard( PT_OVERSCAN, "Overscan" );
	standard( PT_BRIGHTNESS, "Brightness" );
	SetParamGroup( PT_PHOSPHOR, "Tube" );

	// -- Preset --------------------------------------------------------------
	//
	//Element 0 is Custom. Picking anything else lays that row over the bench
	//at read time; nothing is written into params[].
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, static_cast< unsigned int >( 1 + i ), presets::kPresets[ i ].name,
		                     static_cast< float >( 1 + i ) );
	SetParamGroup( PT_PRESET, "Preset" );

	// -- About ---------------------------------------------------------------
	//
	//Declared inline rather than through a helper: SetParamInfo is protected on
	//CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	SetParamGroup( PT_ABOUT_TEXT, "About" );
}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------

float AstablePlugin::Effective( unsigned int index ) const
{
	if( index >= PT_COUNT )
		return 0.0f;

	const int preset = Option( params[ PT_PRESET ], 1 + presets::kCount );
	const int column = PresetColumn( index );
	if( preset > 0 && column >= 0 )
		return presets::kPresets[ preset - 1 ].v[ static_cast< size_t >( column ) ];

	return params[ index ];
}

FFResult AstablePlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	//An About button is a press, not a value to keep: it opens a browser.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float AstablePlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

FFResult AstablePlugin::SetTextParameter( unsigned int index, const char* )
{
	//The About block is display-only, and returning FF_SUCCESS here is not
	//politeness. The SDK's instantiateGL sets EVERY parameter's default on a
	//fresh instance and deletes the instance if any set returns FF_FAIL -- and
	//the base class's SetTextParameter is a stub that returns FF_FAIL. Without
	//this branch no real host can instantiate the plugin at all, while every
	//offline harness that calls the class directly passes happily.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return FF_FAIL;
}

char* AstablePlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		//The host is handed a bare pointer, so the string is kept as a member
		//rather than built on the stack here.
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return const_cast< char* >( "" );
}

FFResult AstablePlugin::SetTime( double time )
{
	lastHostTime = time;
	return CFFGLPlugin::SetTime( time );
}

//---------------------------------------------------------------------------
char* AstablePlugin::GetParameterDisplay( unsigned int index )
{
	//A plain number, for anything without a unit, and the fallback. Answered
	//here rather than by the base class, which dereferences `m_pPlugin` -- a
	//pointer only the host's factory ever sets -- and segfaults the moment the
	//harness constructs the plugin directly.
	auto plain = [ this ]( unsigned int id ) -> char* {
		char buffer[ 32 ] = {};
		std::snprintf( buffer, sizeof( buffer ), "%.4f", GetFloatParameter( id ) );
		displayValue = buffer;
		return displayValue.data();
	};

	if( index >= PT_COUNT )
		return plain( 0 );
	const unsigned int type = GetParamType( index );
	if( type == FF_TYPE_TEXT )
		return GetTextParameter( index );
	if( type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_BUFFER )
		return plain( index );

	//Every number below is read off Resolve(), the same call the engine is
	//driven from, rather than recomputed: a display that re-derives its own
	//mapping is a second copy of it, and the two drift. Fed the SLIDER values
	//and not the preset overlay, because the panel is reporting what the
	//control is set to.
	const Resolved r = Resolve( params, 0.0f );

	char buffer[ 64 ] = {};
	auto ohms = []( char* out, size_t size, double r ) {
		if( r >= 1e6 )
			std::snprintf( out, size, "%.2f MR", r / 1e6 );
		else
			std::snprintf( out, size, "%.1f kR", r / 1e3 );
	};
	auto seconds = []( char* out, size_t size, double s ) {
		if( s <= 0.0 )
			std::snprintf( out, size, "off" );
		else if( s < 1e-3 )
			std::snprintf( out, size, "%.0f us", s * 1e6 );
		else if( s < 1.0 )
			std::snprintf( out, size, "%.2f ms", s * 1e3 );
		else
			std::snprintf( out, size, "%.2f s", s );
	};
	auto farads = []( char* out, size_t size, double f ) {
		if( f < 1e-6 )
			std::snprintf( out, size, "%.1f nF", f * 1e9 );
		else
			std::snprintf( out, size, "%.2f uF", f * 1e6 );
	};

	if( index <= PT_CHANNEL_LAST )
	{
		const int channel        = static_cast< int >( ( index - PT_CHANNEL_FIRST ) / CC_COUNT );
		const ChannelControl ctl = static_cast< ChannelControl >( ( index - PT_CHANNEL_FIRST ) % CC_COUNT );
		const ChannelSpec& ch    = r.bench.channel[ channel ];
		switch( ctl )
		{
		case CC_RA:
			ohms( buffer, sizeof( buffer ), ResistanceOf( params[ index ] ) );
			break;
		case CC_RB:
			ohms( buffer, sizeof( buffer ), ResistanceOf( params[ index ] ) );
			break;
		case CC_C_FINE:
			//The capacitor that results, because "x1.03" is only useful next to
			//the decade it multiplies.
			farads( buffer, sizeof( buffer ), ch.c );
			break;
		case CC_CV_DEPTH:
			std::snprintf( buffer, sizeof( buffer ), "%.0f%%", ch.cvDepth * 100.0f );
			break;
		case CC_MARK_SPACE:
			std::snprintf( buffer, sizeof( buffer ), "%.0f%% high", ch.rCharge / ( ch.rCharge + ch.rDischarge ) * 100.0 );
			break;
		case CC_FILTER:
			seconds( buffer, sizeof( buffer ), ch.filterTau );
			break;
		case CC_LEVEL:
			std::snprintf( buffer, sizeof( buffer ), "%.0f%%", ch.level * 100.0f );
			break;
		default:
			return plain( index );
		}
		displayValue = buffer;
		return displayValue.data();
	}

	switch( index )
	{
	case PT_X_GAIN:
		std::snprintf( buffer, sizeof( buffer ), "x%.2f", r.bench.patch.xGain );
		break;
	case PT_Y_GAIN:
		std::snprintf( buffer, sizeof( buffer ), "x%.2f", r.bench.patch.yGain );
		break;
	case PT_X_OFFSET:
		std::snprintf( buffer, sizeof( buffer ), "%+.2f", r.bench.patch.xOffset );
		break;
	case PT_Y_OFFSET:
		std::snprintf( buffer, sizeof( buffer ), "%+.2f", r.bench.patch.yOffset );
		break;
	case PT_COIL_X:
		seconds( buffer, sizeof( buffer ), r.bench.yoke.coilX );
		break;
	case PT_COIL_Y:
		seconds( buffer, sizeof( buffer ), r.bench.yoke.coilY );
		break;
	case PT_DEFLECTION_GAIN:
		std::snprintf( buffer, sizeof( buffer ), "x%.2f", r.bench.yoke.deflectionGain );
		break;
	case PT_AMP_RAIL:
		std::snprintf( buffer, sizeof( buffer ), "%.2f V", r.bench.yoke.rail );
		break;
	case PT_VCC:
		std::snprintf( buffer, sizeof( buffer ), "%.1f V", r.bench.channel[ 0 ].vcc );
		break;
	case PT_AUDIO_GAIN:
		std::snprintf( buffer, sizeof( buffer ), "x%.2f", Linear( params[ PT_AUDIO_GAIN ], 0.0f, 4.0f ) );
		break;
	case PT_PERSISTENCE:
		std::snprintf( buffer, sizeof( buffer ), "x%.2g", r.tube.persistence );
		break;
	case PT_FACE_ASPECT:
		std::snprintf( buffer, sizeof( buffer ), "%.2f:1", r.tube.faceAspect );
		break;
	case PT_OVERSCAN:
		std::snprintf( buffer, sizeof( buffer ), "%.0f%%", r.tube.deflectionGain * 100.0f );
		break;
	default:
		return plain( index );
	}

	displayValue = buffer;
	return displayValue.data();
}

//---------------------------------------------------------------------------
// Audio
//---------------------------------------------------------------------------

void AstablePlugin::SetAudioForTest( float level )
{
	//The plugin folds the bins as the mean of their square roots, so a flat
	//spectrum of level^2 in every bin reads back as `level`.
	ParamInfo* info = FindParamInfo( PT_AUDIO_FFT );
	if( info == nullptr )
		return;
	const float bin = std::clamp( level, 0.0f, 1.0f );
	for( auto& element : info->elements )
		element.value = bin * bin;
	audioInjected = true;
}

void AstablePlugin::updateAudio()
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO_FFT );
	if( info == nullptr )
		return;

	//sqrt because bin magnitudes bunch hard against zero: a spectrum used raw
	//moves the picture for the kick drum and for nothing else. The mean rather
	//than the peak, because a speaker's whole output is what a real timer's
	//pin 5 would be hearing.
	float sum = 0.0f;
	int counted = 0;
	const size_t bins = std::min< size_t >( info->elements.size(), static_cast< size_t >( kAudioBins ) );
	for( size_t i = 0; i < bins; ++i )
	{
		sum += std::sqrt( std::max( 0.0f, info->elements[ i ].value ) );
		++counted;
	}
	const float raw = counted > 0 ? std::clamp( sum / static_cast< float >( counted ), 0.0f, 1.0f ) : 0.0f;

	//Fast up, slow down: 50 ms of release, so a transient reaches the timer at
	//once and the picture does not snap back between beats.
	const double now = clock.Now();
	const double dt  = ( audioClock >= 0.0 && now > audioClock ) ? now - audioClock : 0.0;
	audioClock       = now;
	if( raw >= audioLevel || dt <= 0.0 )
		audioLevel = raw;
	else
		audioLevel += ( raw - audioLevel ) * static_cast< float >( 1.0 - std::exp( -dt / 0.05 ) );
}

//---------------------------------------------------------------------------
// Resolving
//---------------------------------------------------------------------------

Resolved AstablePlugin::resolve() const
{
	float effective[ PT_COUNT ];
	for( unsigned int id = 0; id < PT_COUNT; ++id )
		effective[ id ] = Effective( id );
	return Resolve( effective, audioLevel );
}

BeamGeometry::RenderParams AstablePlugin::renderParams( const TubeSettings& tube, double frameSeconds ) const
{
	BeamGeometry::RenderParams rp;

	rp.beamPower   = tube.beamPower;
	rp.spotSigma   = tube.spotSigma;
	rp.spotDefocus = kSpotDefocus;
	rp.blankFloor  = 0.0f;

	rp.phosphor    = tube.phosphor;
	rp.persistence = tube.persistence;

	rp.halation          = kHalation;
	rp.halationRadius    = kHalationRadius;
	//Set explicitly rather than inherited from the header's 0.5: a moving
	//trace never reaches 0.5, and at that knee both of vectrix's halation
	//controls were measurably dead while every line of the bloom chain worked.
	rp.halationThreshold = kHalationThreshold;

	rp.tube.faceAspect     = tube.faceAspect;
	rp.tube.cornerRadius   = tube.cornerRadius;
	rp.tube.deflectionGain = tube.deflectionGain;
	rp.tube.curvature      = kCurvature;
	rp.tube.vignette       = kVignette;

	//A television has no graticule and no contrast filter; its face is the
	//object, so the faceplate is fully in front and the layer below shows
	//around it.
	rp.graticule = 0.0f;
	rp.faceBlack = 1.0f;
	rp.opacity   = 1.0f;

	rp.frameSeconds = static_cast< float >( frameSeconds );
	return rp;
}

//---------------------------------------------------------------------------
// GL
//---------------------------------------------------------------------------

FFResult AstablePlugin::InitGL( const FFGLViewportStruct* )
{
	auto glString = []( GLenum name ) {
		const GLubyte* s = glGetString( name );
		return s != nullptr ? std::string( reinterpret_cast< const char* >( s ) ) : std::string( "?" );
	};
	diag::info( "GL vendor=" + glString( GL_VENDOR ) + " renderer=" + glString( GL_RENDERER )
	            + " version=" + glString( GL_VERSION ) );

	if( !beam.InitGL() )
	{
		diag::error( "the beam renderer would not initialise" );
		return FF_FAIL;
	}

	glReady = true;
	return FF_SUCCESS;
}

FFResult AstablePlugin::DeInitGL()
{
	beam.DeInitGL();
	glReady = false;
	return FF_SUCCESS;
}

FFResult AstablePlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( !glReady || pGL == nullptr )
		return FF_FAIL;

	//The host's viewport, read fresh rather than remembered from InitGL:
	//Resolume changes composition resolution without reinitialising a plugin.
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, viewport );
	if( viewport[ 2 ] <= 0 || viewport[ 3 ] <= 0 )
		return FF_FAIL;

	clock.Update( lastHostTime );
	updateAudio();

	const Resolved resolved = resolve();
	bench.SetParams( resolved.bench );

	const int n           = clock.SamplesForThisFrame( bench.SampleRate() );
	const Sample* samples = bench.Render( n, clock.FrameSeconds() );

	const bool drawn = beam.Render( samples, n, renderParams( resolved.tube, clock.FrameSeconds() ),
	                                pGL->HostFBO, viewport, 0, 1.0f, 1.0f );

	if( ++frameCounter == 60 )
	{
		//Logged once, because it settles an argument about which unit a host
		//actually sent -- and no offline harness can answer it, since the
		//harness is the thing sending seconds.
		diag::info( std::string( "host clock scale " )
		            + ( clock.ClockScale() == 0.001 ? "0.001 (milliseconds)" : "1.0 (seconds)" )
		            + ", engine rate " + std::to_string( static_cast< int >( bench.SampleRate() ) ) + " Hz" );
	}

	return drawn ? FF_SUCCESS : FF_FAIL;
}

} // namespace astable
