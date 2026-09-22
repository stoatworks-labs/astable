/**
    attest -- the offline harness.

    It drives **the real code that ships** in a headless core-profile context:
    `AstablePlugin` for anything that goes through the parameter list, `Bench`
    and `Timer555` directly for the circuit checks -- which need an exact 4.7
    kR rather than a slider position -- and `BeamGeometry` directly where the
    picture has to be measured without the tube's fixed decorations on it.
    Nothing below is a reimplementation: every number printed comes out of the
    simulation that runs in Resolume, or a frame that was actually rendered.

        --period     ten (Ra, Rb, C) triples: period = 0.693 (Ra + 2Rb) C and
                     duty = (Ra + Rb) / (Ra + 2Rb), both within 1%
        --swing      the capacitor runs between V5/2 and V5 within 1%, with
                     V5 at rest (2/3 Vcc) and driven
        --recover    a channel whose pin 5 has been driven comes back at the
                     datasheet period once the CV is removed, or pin 4 pulsed
        --markspace  Mark-Space moves the duty and holds the period within 0.5%
        --dots       two squares into X and Y: >90% of the light in four spots
        --yoke       a step into a coil with time constant tau is the exponential
                     it should be: the fitted tau within 1%, 63% at tau
        --energy     vectrix's: total light independent of sweep speed within 0.5%
        --presets    every preset renders, all distinct, row 1 = Custom
        --defaults   preset row 1 IS the constructor's defaults (no GL)
        --names      names and displays fit FFGL's 16 characters (no GL)
        --bench      ms/frame at 720p, 1080p and 4K
        --all        every check above, with a summary

        --out PATH   render a frame     --size WxH   --frames N   --preset N
        --list       every parameter    --set "Name=value" (repeatable)
        --audio L    a flat spectrum whose folded level is L, 0..1
        --pipe       raw RGBA frames on stdout, for the video pipeline

    ## --pipe

    The fleet's frame format, so one filming script can drive any of the FFGL
    plugins. Astable is a **source**: it declares zero inputs and reads nothing,
    so unlike tinsel's or porthole's this end of the pipe has no stdin side.
    Nothing is read; frames are written until `--frames` is reached, or until
    the reader closes the pipe if no count was asked for.

        attest --pipe --width 1920 --height 1080 --frames 600 [--script cues.txt] \
          | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - out.mov

    `--script` is a plain text file of `frame  Parameter Name  value` lines --
    `frame  Name=value` is accepted too -- held before the first key and after
    the last, and linearly interpolated between. Note what interpolating means
    for an OPTION parameter: moving Preset from Four Dots to Raster passes
    through every row between them, so key such a parameter one frame apart to
    cut, and give it a hold key at the END of every section it must not move in.

    A name that is not a parameter is refused rather than ignored, because a
    misspelled name that silently did nothing would produce a take that looks
    deliberate and is wrong -- the reel would hold whatever the default was,
    under a caption describing a control that never moved.

    ## Determinism

    Time comes from the frame counter and never from a wall clock: every
    render calls `SetTime( frame / 60 )` first, which pins `FrameSeconds` to
    exactly 1/60 through the clamp and therefore pins the sample count. Two
    runs of the same command produce byte-identical PNGs.

    ## What each check can and cannot catch

    `--period` is the check the model exists to pass, and it is measured from
    the sample stream the yoke would see -- edges in `Sample::x` -- rather than
    from the flip-flop's own clock, so a comparator that switched at the right
    instant but a patch bay that sampled it wrongly would fail. The flip-flop's
    exact figure is printed beside it. The ten triples span 1 Hz to 20 kHz,
    which is the range a yoke can follow; the extremes of the part ranges
    (2 us and 200 s periods) are checked separately against the flip-flop's
    clock, because at 480 kHz the engine has under one sample a period and a
    sample-quantised measurement would be measuring the quantisation.

    `--dots` renders the bench's own sample block through `BeamGeometry` with
    the halation off, and so proves the engine and the renderer together but
    not the fixed tube decorations a real frame carries. `--presets` renders
    through the plugin and covers those.

    None of them catches a uniform whose name does not match the GLSL, because
    `glUniform` with location -1 is a documented no-op. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Astable.h"
#include "Controls.h"
#include "Presets.h"
#include "render/BeamGeometry.h"
#include "render/Phosphor.h"
#include "render/Tube.h"
#include "signal/Bench.h"
#include "signal/Signal.h"
#include "signal/Timer555.h"

using namespace astable;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );// filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

	std::vector< unsigned char > header;
	putU32( header, static_cast< uint32_t >( width ) );
	putU32( header, static_cast< uint32_t >( height ) );
	header.push_back( 8 );// bit depth
	header.push_back( 6 );// colour type: RGBA
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );
	putChunk( png, "IHDR", header );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
	bool floating  = false;
};

/// `floating` asks for RGBA32F rather than RGBA8. The invariant tests sum a
/// whole frame and quote fractions of a percent; eight bits per channel would
/// put the measurement's own noise floor next to the tolerance.
Target makeTarget( int width, int height, bool floating = false )
{
	Target target;
	target.width    = width;
	target.height   = height;
	target.floating = floating;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, floating ? GL_RGBA32F : GL_RGBA8, width, height, 0,
	              GL_RGBA, floating ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		std::fprintf( stderr, "the %dx%d %s target is not framebuffer-complete\n",
		              width, height, floating ? "float" : "8-bit" );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

/// Straight out of GL, **bottom row first**.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< float > readFloats( const Target& target )
{
	std::vector< float > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// The harness's clock. 1/60 exactly: inside Clock's [1/240, 1/24] clamp, so
// the clamp never fires and the sample count is a function of the rate alone.
//---------------------------------------------------------------------------
constexpr double kFrameSeconds = 1.0 / 60.0;

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
bool startPlugin( AstablePlugin& plugin, const Target& target )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	//Once per instance, never per frame: BeamGeometry::InitGL compiles five
	//shaders and generates a VAO and a VBO without deleting the previous set.
	return plugin.InitGL( &viewport ) == FF_SUCCESS;
}

/// `frameSeconds` is how long the plugin is told this frame lasted. It is a
/// parameter only because `--pipe` takes an `--fps`: everything else here runs
/// at 1/60 exactly, which sits inside `Clock`'s [1/240, 1/24] clamp so the
/// clamp never fires and the sample count is a function of the engine rate
/// alone. A `--fps` outside that window IS clamped, and the reel then advances
/// in engine time more slowly (or faster) than its own frame numbering says --
/// which is a real filming decision rather than a bug, and is why the clamp is
/// named here rather than hidden.
bool renderFrame( AstablePlugin& plugin, const Target& target, int frameIndex,
                  double frameSeconds = kFrameSeconds )
{
	//The whole of the harness's determinism is these two lines.
	plugin.SetClockScaleForTest( 1.0 );
	plugin.SetTime( static_cast< double >( frameIndex ) * frameSeconds );

	ProcessOpenGLStruct process {};
	process.numInputTextures = 0;
	process.inputTextures    = nullptr;
	process.HostFBO          = target.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

/// Every parameter's host-facing name, read out of the plugin itself, so a
/// `--set` that addresses nothing is an error rather than a silent no-op.
std::map< std::string, unsigned int > parameterIndex( AstablePlugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

int applySets( AstablePlugin& plugin, const std::vector< std::pair< std::string, float > >& sets )
{
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	int unresolved = 0;

	for( const auto& set : sets )
	{
		char* end           = nullptr;
		const long asNumber = std::strtol( set.first.c_str(), &end, 10 );
		unsigned int id     = PT_COUNT;

		if( end != nullptr && *end == '\0' && !set.first.empty() && asNumber >= 0 && asNumber < PT_COUNT )
			id = static_cast< unsigned int >( asNumber );
		else
		{
			const auto found = byName.find( set.first );
			if( found != byName.end() )
				id = found->second;
		}

		if( id >= PT_COUNT )
		{
			std::fprintf( stderr, "attest: no parameter called \"%s\"\n", set.first.c_str() );
			++unresolved;
			continue;
		}
		plugin.SetFloatParameter( id, set.second );
	}
	return unresolved;
}

/// The plugin's shipped defaults, as a params array, for anything that wants
/// to go through Resolve() without a host.
std::vector< float > defaultParams()
{
	AstablePlugin plugin;
	std::vector< float > out( PT_COUNT );
	for( unsigned int id = 0; id < PT_COUNT; ++id )
		out[ id ] = plugin.GetFloatParameter( id );
	return out;
}

//---------------------------------------------------------------------------
// The renderer bench: BeamGeometry with a sample block the harness controls.
//---------------------------------------------------------------------------
/// P31, index 2 in this plugin's table: efficiency 1.0, green exactly 1.0,
/// saturation 12 -- so the green channel IS the saturated excitation with no
/// constant to divide out. The bench is calibrated against it, as vectrix's is.
constexpr int kBenchPhosphor = 2;

BeamGeometry::RenderParams benchParams( float sigma, float beamPower )
{
	BeamGeometry::RenderParams rp;
	rp.beamPower    = beamPower;
	rp.spotSigma    = sigma;
	rp.spotDefocus  = 0.0f;
	rp.blankFloor   = 0.0f;
	rp.densityFloor = 1.0e-4f;

	rp.phosphor    = kBenchPhosphor;
	rp.persistence = 1.0f;

	rp.halation  = 0.0f;
	rp.graticule = 0.0f;
	rp.faceBlack = 0.0f;
	rp.opacity   = 1.0f;

	rp.tube.faceAspect     = 1.0f;
	rp.tube.cornerRadius   = 0.0f;
	rp.tube.deflectionGain = 1.0f;
	rp.tube.curvature      = 0.0f;
	rp.tube.vignette       = 0.0f;

	rp.frameSeconds = static_cast< float >( kFrameSeconds );
	rp.clearHistory = true;
	return rp;
}

/// The face buffer's size for this sigma, and therefore the output size to
/// render at, so that an output pixel centre lands on a face texel centre.
int benchSize( float sigma )
{
	return faceSizeFor( sigma * 0.5f, 2160 );
}

double excitationFrom( double light, double saturation )
{
	if( !( light > 0.0 ) )
		return 0.0;
	if( light >= saturation )
		return std::numeric_limits< double >::infinity();
	return light / ( 1.0 - light / saturation );
}

double totalLight( const std::vector< float >& pixels )
{
	double sum = 0.0;
	for( size_t i = 0; i < pixels.size(); i += 4 )
		sum += static_cast< double >( pixels[ i ] ) + pixels[ i + 1 ] + pixels[ i + 2 ];
	return sum;
}

//---------------------------------------------------------------------------
// Edges in a sample stream, for the timing checks.
//---------------------------------------------------------------------------
struct Edges
{
	std::vector< double > rises;
	std::vector< double > falls;
};

/// Rising and falling crossings of the midpoint between the stream's extremes,
/// with the time of each taken as the sample it was first seen on.
Edges findEdges( const std::vector< double >& t, const std::vector< double >& x )
{
	Edges edges;
	if( x.size() < 4 )
		return edges;
	double lo = *std::min_element( x.begin(), x.end() );
	double hi = *std::max_element( x.begin(), x.end() );
	const double mid = 0.5 * ( lo + hi );
	for( size_t i = 1; i < x.size(); ++i )
	{
		if( x[ i - 1 ] < mid && x[ i ] >= mid )
			edges.rises.push_back( t[ i ] );
		else if( x[ i - 1 ] >= mid && x[ i ] < mid )
			edges.falls.push_back( t[ i ] );
	}
	return edges;
}

/// Mean period and duty over complete cycles, skipping the first `skip` so
/// the power-up transient -- the first charge starts from an empty capacitor
/// -- is not in the average.
bool measureCycles( const Edges& edges, size_t skip, double& period, double& duty, int& cycles )
{
	if( edges.rises.size() < skip + 3 )
		return false;
	double sumPeriod = 0.0, sumHigh = 0.0;
	cycles = 0;
	for( size_t i = skip + 1; i < edges.rises.size(); ++i )
	{
		const double rise = edges.rises[ i - 1 ];
		const double next = edges.rises[ i ];
		//The fall between the two rises.
		auto fall = std::upper_bound( edges.falls.begin(), edges.falls.end(), rise );
		if( fall == edges.falls.end() || *fall >= next )
			continue;
		sumPeriod += next - rise;
		sumHigh += *fall - rise;
		++cycles;
	}
	if( cycles < 3 )
		return false;
	period = sumPeriod / cycles;
	duty   = sumHigh / sumPeriod;
	return true;
}

/// Run the bench for `seconds` of engine time in 1/60 s frames at its own
/// rate, collecting x (and optionally y) with timestamps.
void runBench( Bench& bench, double seconds, std::vector< double >& t, std::vector< double >& x,
               std::vector< double >* y = nullptr )
{
	double now = 0.0;
	while( now < seconds )
	{
		const int n           = std::clamp( static_cast< int >( std::lround( kFrameSeconds * bench.SampleRate() ) ), 2, kMaxBlock );
		const Sample* samples = bench.Render( n, kFrameSeconds );
		const double dt       = kFrameSeconds / static_cast< double >( n - 1 );
		//Sample 0 repeats the previous block's last sample; skip it after the
		//first block so no instant appears twice.
		for( int i = ( t.empty() ? 0 : 1 ); i < n; ++i )
		{
			t.push_back( now + i * dt );
			x.push_back( samples[ i ].x );
			if( y != nullptr )
				y->push_back( samples[ i ].y );
		}
		now += kFrameSeconds;
	}
}

/// A bench with channel 1 set to exact parts and patched straight to X, the
/// amplifier at unity with no coil, no filter and a rail it never reaches.
BenchParams benchWith( double ra, double rb, double c, double vcc = 9.0 )
{
	BenchParams bp = Resolve( defaultParams().data(), 0.0f ).bench;
	ChannelSpec& ch  = bp.channel[ 0 ];
	ch.rCharge       = ra + rb;
	ch.rDischarge    = rb;
	ch.c             = c;
	ch.vcc           = vcc;
	ch.nominalPeriod = NominalPeriod( ra, rb, c );
	ch.filterTau     = 0.0;
	ch.cvSource      = 0;
	ch.level         = 1.0f;
	//Only channel 1 runs, so the rate follows it and nothing else.
	for( int i = 1; i < kChannels; ++i )
		bp.channel[ i ].reset = false;
	bp.patch.xSource = 1;
	bp.patch.ySource = 0;
	bp.patch.zSource = 0;
	bp.patch.xGain   = 1.0f;
	bp.patch.xOffset = 0.0f;
	bp.yoke.coilX    = 0.0;
	bp.yoke.coilY    = 0.0;
	bp.yoke.deflectionGain = 1.0f;
	bp.yoke.rail     = 3.0f;
	return bp;
}

//---------------------------------------------------------------------------
// --period
//---------------------------------------------------------------------------
int checkPeriod()
{
	struct Triple
	{
		double ra, rb, c;
	};

	//Ten triples across the ranges a yoke can follow -- 120 Hz to 34 kHz, and
	//down to 0.3 Hz where the figure is a slow crawl -- with duties from 50.5%
	//(Rb >> Ra) to 99.8% (Ra >> Rb) and every capacitor decade from 1 nF to
	//10 uF. The part extremes are checked separately below.
	std::vector< Triple > cases = {
		{ 2.2e3, 1e3, 10e-9 },    // 34 kHz, the fast end a yoke could follow
		{ 10e3, 4.7e3, 100e-9 },
		{ 4.7e3, 2.2e3, 100e-9 },
		{ 22e3, 22e3, 10e-9 },
		{ 2.2e3, 100e3, 1e-9 },
		{ 100e3, 47e3, 1e-6 },
		{ 47e3, 10e3, 10e-6 },
		{ 470e3, 1e3, 10e-6 },
		{ 1e6, 220e3, 1e-6 },
		{ 10e3, 1e3, 1e-6 },      // 120 Hz, duty 92%
	};

	std::printf( "  period = 0.693 (Ra + 2Rb) C and duty = (Ra + Rb) / (Ra + 2Rb), measured from\n"
	             "  the edges the yoke sees in Sample::x, over complete cycles after the first five\n\n" );
	std::printf( "  %8s %8s %8s | %10s %10s %8s | %7s %7s %8s | %8s\n",
	             "Ra", "Rb", "C", "period", "measured", "err", "duty", "meas", "err", "exact" );

	int failures = 0;
	for( const Triple& tr : cases )
	{
		const double expectedPeriod = NominalPeriod( tr.ra, tr.rb, tr.c );
		const double expectedDuty   = ( tr.ra + tr.rb ) / ( tr.ra + 2.0 * tr.rb );

		Bench bench;
		bench.Prepare();
		bench.SetParams( benchWith( tr.ra, tr.rb, tr.c ) );

		//Forty cycles, or at least a second, whichever is longer.
		const double seconds = std::max( 40.0 * expectedPeriod, 1.0 ) + kFrameSeconds;
		std::vector< double > t, x;
		runBench( bench, seconds, t, x );

		double period = 0.0, duty = 0.0;
		int cycles = 0;
		const bool ok = measureCycles( findEdges( t, x ), 5, period, duty, cycles );

		const double periodErr = ok ? ( period / expectedPeriod - 1.0 ) * 100.0 : 100.0;
		const double dutyErr   = ok ? ( duty - expectedDuty ) * 100.0 : 100.0;
		const double exact     = bench.Timer( 0 ).LastPeriod();

		std::printf( "  %8.3g %8.3g %8.3g | %10.4g %10.4g %+7.3f%% | %6.2f%% %6.2f%% %+7.3f%% | %+7.4f%%\n",
		             tr.ra, tr.rb, tr.c, expectedPeriod, period, periodErr,
		             expectedDuty * 100.0, duty * 100.0, dutyErr,
		             ( exact / expectedPeriod - 1.0 ) * 100.0 );

		if( !ok )
		{
			std::fprintf( stderr, "period: too few cycles measured for %g/%g/%g (%d)\n", tr.ra, tr.rb, tr.c, cycles );
			++failures;
		}
		else if( std::fabs( periodErr ) > 1.0 || std::fabs( dutyErr ) > 1.0 )
		{
			std::fprintf( stderr, "period: %g/%g/%g is off by %.3f%% (period) / %.3f points (duty), tolerance 1%%\n",
			              tr.ra, tr.rb, tr.c, periodErr, dutyErr );
			++failures;
		}
	}

	//The extremes of the part ranges, against the flip-flop's own crossing
	//times rather than the sample stream: at 1 kR / 1 kR / 1 nF the period is
	//2 us, which is under one sample at the engine's 384 kHz cap, so the yoke
	//cannot see it at all -- but the comparators must still keep time, because
	//that is what stops a fast channel's PHASE drifting while it is being used
	//as a CV source for a slow one.
	std::printf( "\n  the extremes of the part ranges, from the flip-flop's exact crossing times:\n" );
	const Triple extremes[] = { { 1e3, 1e3, 1e-9 }, { 1e6, 1e6, 100e-6 } };
	for( const Triple& tr : extremes )
	{
		const double expected = NominalPeriod( tr.ra, tr.rb, tr.c );
		Timer555 timer;
		Timer555::Params tp;
		tp.rCharge    = tr.ra + tr.rb;
		tp.rDischarge = tr.rb;
		tp.c          = tr.c;
		tp.vcc        = 9.0;
		timer.SetParams( tp );
		timer.Reset();

		//Thirty periods at 96 samples each, whatever the period is: the state
		//is a voltage, so the wall-clock cost of a 208-second timer is the
		//same as a 2-microsecond one.
		const double dt = expected / Bench::kSamplesPerPeriod;
		const long steps = static_cast< long >( 30.0 * Bench::kSamplesPerPeriod );
		for( long i = 0; i < steps; ++i )
			timer.Step( dt, 6.0 );
		const double err = ( timer.LastPeriod() / expected - 1.0 ) * 100.0;
		std::printf( "  %8.3g %8.3g %8.3g | %10.4g %10.4g %+7.4f%%  (%.0f samples a period)\n",
		             tr.ra, tr.rb, tr.c, expected, timer.LastPeriod(), err, Bench::kSamplesPerPeriod );
		if( std::fabs( err ) > 0.1 )
		{
			std::fprintf( stderr, "period: the flip-flop's own period is off by %.4f%% at %g/%g/%g\n", err, tr.ra, tr.rb, tr.c );
			++failures;
		}
	}

	//And the same fast part driven at the engine's own cap, where one interval
	//spans more than a whole period. This is the case the closed-form crossing
	//solve exists for: a comparator sampled once per interval would report a
	//period of one interval and the beat between two such channels would be an
	//artefact of the sample rate rather than of the parts.
	{
		const double expected = NominalPeriod( 1e3, 1e3, 1e-9 );
		Timer555 timer;
		Timer555::Params tp;
		tp.rCharge    = 2e3;
		tp.rDischarge = 1e3;
		tp.c          = 1e-9;
		tp.vcc        = 9.0;
		timer.SetParams( tp );
		timer.Reset();

		const double dt = 1.0 / Bench::kMaxRate;
		for( long i = 0; i < 4000; ++i )
			timer.Step( dt, 6.0 );
		const double err = ( timer.LastPeriod() / expected - 1.0 ) * 100.0;
		std::printf( "  the same 2.08 us part at the engine's %.0f kHz cap -- %.2f samples a period:\n"
		             "  %10.4g measured against %10.4g, %+.4f%%\n",
		             Bench::kMaxRate / 1000.0, expected / dt, timer.LastPeriod(), expected, err );
		if( std::fabs( err ) > 0.1 )
		{
			std::fprintf( stderr, "period: under-sampled, the period is off by %.4f%%\n", err );
			++failures;
		}
	}

	std::printf( failures == 0 ? "period: ok\n" : "period: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --swing
//---------------------------------------------------------------------------
int checkSwing()
{
	constexpr double kVcc = 9.0;
	const double ra = 10e3, rb = 4.7e3, c = 100e-9;
	const double period = NominalPeriod( ra, rb, c );

	struct Case
	{
		const char* label;
		double v5;
	};
	const Case cases[] = {
		{ "pin 5 at rest, 2/3 Vcc", kVcc * 2.0 / 3.0 },
		{ "pin 5 pulled down to Vcc/2", kVcc * 0.5 },
		{ "pin 5 pulled up to 5/6 Vcc", kVcc * 5.0 / 6.0 },
	};

	std::printf( "  Vcc = %.1f V. The capacitor must run between V5/2 and V5 -- Vcc/3 and 2Vcc/3\n"
	             "  at rest -- and follow pin 5 when it is driven. Sampled at 1024 a period, so the\n"
	             "  extreme seen is within 0.1%% of the extreme reached; at the engine's 96 the\n"
	             "  sample after a crossing has already moved on, which is quoted, not asserted.\n\n",
	             kVcc );

	int failures = 0;
	for( const Case& cs : cases )
	{
		double results[ 2 ][ 2 ] = {};//[fine/coarse][min/max]
		const double rates[ 2 ]  = { 1024.0, Bench::kSamplesPerPeriod };
		for( int r = 0; r < 2; ++r )
		{
			Timer555 timer;
			Timer555::Params tp;
			tp.rCharge    = ra + rb;
			tp.rDischarge = rb;
			tp.c          = c;
			tp.vcc        = kVcc;
			timer.SetParams( tp );
			timer.Reset();

			const double dt = period / rates[ r ];
			double lo = 1e9, hi = -1e9;
			const long settle = static_cast< long >( 10.0 * rates[ r ] );
			const long total  = static_cast< long >( 40.0 * rates[ r ] );
			for( long i = 0; i < total; ++i )
			{
				timer.Step( dt, cs.v5 );
				if( i >= settle )
				{
					lo = std::min( lo, timer.Cap() );
					hi = std::max( hi, timer.Cap() );
				}
			}
			results[ r ][ 0 ] = lo;
			results[ r ][ 1 ] = hi;
		}

		const double wantLo = 0.5 * cs.v5, wantHi = cs.v5;
		const double loErr  = ( results[ 0 ][ 0 ] / wantLo - 1.0 ) * 100.0;
		const double hiErr  = ( results[ 0 ][ 1 ] / wantHi - 1.0 ) * 100.0;
		std::printf( "  %-28s want %.4f .. %.4f V   got %.4f .. %.4f (%+.3f%%, %+.3f%%)   at 96/period %.4f .. %.4f\n",
		             cs.label, wantLo, wantHi, results[ 0 ][ 0 ], results[ 0 ][ 1 ], loErr, hiErr,
		             results[ 1 ][ 0 ], results[ 1 ][ 1 ] );
		if( std::fabs( loErr ) > 1.0 || std::fabs( hiErr ) > 1.0 )
		{
			std::fprintf( stderr, "swing: %s is off by %.3f%% / %.3f%%\n", cs.label, loErr, hiErr );
			++failures;
		}
	}

	std::printf( failures == 0 ? "swing: ok\n" : "swing: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --recover
//---------------------------------------------------------------------------
//
// The check `--swing` cannot make. Swing holds pin 5 at a FIXED voltage and
// starts from Reset(), so the comparator levels are already on the far side of
// the capacitor from the rail it is heading for, and they stay there. Nothing
// in it ever moves a level ACROSS the capacitor -- which is the one thing a
// control voltage does that a bias does not, and it was the whole of the bug:
// a threshold pulled down under an already-charged C, or a trigger pushed up
// over an already-drained one, left the part with no crossing to solve for and
// it sat on the rail until the instance was destroyed. Removing the CV did not
// help, because the capacitor was still on the wrong side. Pin 4 did not help
// either, because a reset drains C to ground, which is below the trigger.
//
// So: drive pin 5 hard for three seconds, take it away, and require the part
// to be oscillating at the datasheet period again. Measured from edges counted
// AFTER the CV is removed, never from LastPeriod alone -- a latched timer
// keeps reporting whatever its last complete cycle was, so a stale reading is
// exactly what this has to be able to tell from a live one.
int checkRecover()
{
	constexpr double kVcc  = 9.0;
	constexpr double kRest = kVcc * 2.0 / 3.0;
	const double ra = 10e3, rb = 4.7e3, c = 100e-9;
	const double want = NominalPeriod( ra, rb, c );

	//Seconds, in the simulation's own time.
	constexpr double kDrive   = 3.0;
	constexpr double kSettle  = 0.5;
	constexpr double kMeasure = 1.0;

	enum Shape { kSine, kSquare };
	enum Release { kRemove, kResetPin };

	struct Case
	{
		const char* label;
		Shape shape;
		double ratio;///< The modulation rate as a multiple of the timer's own.
		double depth;
		Release release;
	};

	//Rates either side of the timer's own, because which side decides whether
	//a level sweeps past the capacitor or the capacitor sweeps past the level,
	//and both of them latched. Depths are the range the report came in at,
	//plus a full-depth case that the clamp has to survive.
	const Case cases[] = {
		{ "sine  0.01x, depth 0.12", kSine, 0.01, 0.12, kRemove },
		{ "sine  0.20x, depth 0.22", kSine, 0.20, 0.22, kRemove },
		{ "sine  1.30x, depth 0.04", kSine, 1.30, 0.04, kRemove },
		{ "sine 11.00x, depth 1.00", kSine, 11.00, 1.00, kRemove },
		{ "sq    0.20x, depth 0.12", kSquare, 0.20, 0.12, kRemove },
		{ "sq    1.30x, depth 0.45", kSquare, 1.30, 0.45, kRemove },
		{ "sq   11.00x, depth 1.00", kSquare, 11.00, 1.00, kRemove },
		{ "sine  0.20x, depth 0.22, pin 4", kSine, 0.20, 0.22, kResetPin },
		{ "sq    1.30x, depth 0.45, pin 4", kSquare, 1.30, 0.45, kResetPin },
	};

	std::printf( "  Vcc = %.1f V, Ra = 10 kR, Rb = 4.7 kR, C = 100 nF -- a %.4f ms part at %.1f Hz.\n"
	             "  Pin 5 is driven for %.0f s exactly as Bench::ControlVoltage drives it, then either\n"
	             "  released to 2/3 Vcc or pulsed on pin 4, and the period is re-measured from rising\n"
	             "  edges counted in the %.1f s AFTER that. Tolerance 1%%.\n\n",
	             kVcc, want * 1e3, 1.0 / want, kDrive, kMeasure );

	const double dt = want / Bench::kSamplesPerPeriod;
	int failures    = 0;

	for( const Case& cs : cases )
	{
		Timer555 timer;
		Timer555::Params tp;
		tp.rCharge    = ra + rb;
		tp.rDischarge = rb;
		tp.c          = c;
		tp.vcc        = kVcc;
		timer.SetParams( tp );
		timer.Reset();

		//-- drive -----------------------------------------------------------
		const double modHz = cs.ratio / want;
		const long driven  = static_cast< long >( kDrive / dt );
		for( long i = 0; i < driven; ++i )
		{
			const double t = static_cast< double >( i ) * dt;
			const double phase = modHz * t;
			const double s = cs.shape == kSine ? std::sin( 6.283185307179586 * phase )
			                                   : ( std::fmod( phase, 1.0 ) < 0.5 ? 1.0 : -1.0 );
			//The formula from Bench::ControlVoltage, including its 0.45.
			timer.Step( dt, kRest * ( 1.0 + 0.45 * cs.depth * s ) );
		}

		//-- release ----------------------------------------------------------
		if( cs.release == kResetPin )
		{
			//Pin 4 low for a millisecond, then high again, with pin 5 already
			//back at rest. The operator's "have you tried resetting it".
			tp.reset = false;
			timer.SetParams( tp );
			for( long i = 0; i < static_cast< long >( 1e-3 / dt ); ++i )
				timer.Step( dt, kRest );
			tp.reset = true;
			timer.SetParams( tp );
		}

		for( long i = 0; i < static_cast< long >( kSettle / dt ); ++i )
			timer.Step( dt, kRest );

		//-- measure -----------------------------------------------------------
		const long window = static_cast< long >( kMeasure / dt );
		bool wasHigh      = timer.High();
		long firstEdge = -1, lastEdge = -1;
		long edges = 0;
		for( long i = 0; i < window; ++i )
		{
			timer.Step( dt, kRest );
			const bool isHigh = timer.High();
			if( isHigh && !wasHigh )
			{
				if( firstEdge < 0 )
					firstEdge = i;
				lastEdge = i;
				++edges;
			}
			wasHigh = isHigh;
		}

		const bool alive = edges >= 2;
		//Averaged over every cycle in the window, so the sample quantisation on
		//the two end edges is divided by the cycle count and does not show.
		const double measured = alive ? static_cast< double >( lastEdge - firstEdge ) * dt
		                                  / static_cast< double >( edges - 1 )
		                              : 0.0;
		const double err   = alive ? ( measured / want - 1.0 ) * 100.0 : 0.0;
		const double latch = timer.LastPeriod();

		if( alive )
			std::printf( "  %-32s %5ld edges   %.4f ms (%+.3f%%)   flip-flop %.4f ms\n",
			             cs.label, edges, measured * 1e3, err, latch * 1e3 );
		else
			std::printf( "  %-32s %5ld edges   LATCHED -- C at %.4f V, output %s, flip-flop still says %.4f ms\n",
			             cs.label, edges, timer.Cap(), timer.High() ? "high" : "low", latch * 1e3 );

		if( !alive )
		{
			std::fprintf( stderr, "recover: %s did not restart: %ld edges in %.1f s\n", cs.label, edges, kMeasure );
			++failures;
		}
		else if( std::fabs( err ) > 1.0 )
		{
			std::fprintf( stderr, "recover: %s came back at %+.3f%% of the datasheet period\n", cs.label, err );
			++failures;
		}
	}

	//-- and now the patch an operator actually builds -------------------------
	//
	// Through Bench, so Controls.cpp's mapping and Bench::ControlVoltage are
	// both in the path: channel 1's pin 5 from channel 4's capacitor at a depth
	// in the middle of the reported range, then the depth taken back to zero on
	// a RUNNING bench -- which is what a preset change does, and what the
	// report says never recovered.
	{
		BenchParams bp = Resolve( defaultParams().data(), 0.0f ).bench;
		for( int i = 0; i < kChannels; ++i )
		{
			bp.channel[ i ].cvSource = 0;
			bp.channel[ i ].cvDepth  = 0.0f;
		}
		//kCvSourceNames[ 10 ] is "Ch4 Cap"; asserted rather than assumed,
		//because the list is allowed to grow and this index would move.
		if( std::string( kCvSourceNames[ 10 ] ) != "Ch4 Cap" )
		{
			std::fprintf( stderr, "recover: kCvSourceNames[10] is \"%s\", not \"Ch4 Cap\"\n", kCvSourceNames[ 10 ] );
			++failures;
		}
		bp.channel[ 0 ].cvSource = 10;
		bp.channel[ 0 ].cvDepth  = 0.12f;

		const double nominal = bp.channel[ 0 ].nominalPeriod;

		Bench bench;
		bench.Prepare();
		bench.SetParams( bp );

		//The capacitor's excursion, sampled once a frame. At 60 Hz against a
		//744 Hz timer this aliases all over the cycle, which is all it has to
		//do: a channel that is oscillating visits Vcc/3 .. 2Vcc/3 within a few
		//frames and a latched one sits on a rail and reports nothing. This is
		//the assertion that does not depend on LastPeriod, which a latched
		//timer goes on reporting from the last cycle it completed and which is
		//therefore only ever corroborating evidence here.
		double capLo = 1e9, capHi = -1e9;
		auto run = [ & ]( double seconds, bool watch ) {
			const int n = std::clamp( static_cast< int >( std::lround( kFrameSeconds * bench.SampleRate() ) ), 2, kMaxBlock );
			for( int f = 0; f < static_cast< int >( seconds / kFrameSeconds ); ++f )
			{
				bench.Render( n, kFrameSeconds );
				if( watch )
				{
					capLo = std::min( capLo, bench.Timer( 0 ).Cap() );
					capHi = std::max( capHi, bench.Timer( 0 ).Cap() );
				}
			}
		};

		run( 3.0, false );
		const double driven = bench.Timer( 0 ).LastPeriod();

		//Depth to zero, nothing else touched. SetParams must not restart the
		//timers, so this is the CV being removed from a running circuit.
		bp.channel[ 0 ].cvDepth = 0.0f;
		bench.SetParams( bp );
		run( 0.5, false );
		run( 3.0, true );

		const double back  = bench.Timer( 0 ).LastPeriod();
		const double err   = ( back / nominal - 1.0 ) * 100.0;
		const double vcc   = bp.channel[ 0 ].vcc;
		//Pin 5 is back at rest, so the run is Vcc/3 .. 2Vcc/3. Nine tenths of
		//it, to leave room for the frame sampling missing the very extremes.
		const double wantSpan = 0.9 * vcc / 3.0;
		const double span     = capHi - capLo;
		std::printf( "\n  Ch1 CV from Ch4 Cap at 0.12, then back to 0 on a running bench:\n"
		             "    datasheet %.4f ms   while driven %.4f ms   after %.4f ms (%+.3f%%)\n"
		             "    C over the 3 s after: %.4f .. %.4f V, a %.4f V run (want >= %.4f)\n",
		             nominal * 1e3, driven * 1e3, back * 1e3, err, capLo, capHi, span, wantSpan );
		if( span < wantSpan )
		{
			std::fprintf( stderr, "recover: channel 1 is not oscillating after the CV was removed:"
			                      " C sat in %.4f .. %.4f V\n", capLo, capHi );
			++failures;
		}
		else if( std::fabs( err ) > 1.0 )
		{
			std::fprintf( stderr, "recover: channel 1 came back at %+.3f%% of the datasheet period\n", err );
			++failures;
		}
	}

	std::printf( failures == 0 ? "recover: ok\n" : "recover: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --markspace
//---------------------------------------------------------------------------
//
// Through Controls.cpp, because the pot lives there: the host's 0..1 becomes
// a charge and a discharge resistance whose sum is the stock circuit's.
int checkMarkSpace()
{
	const float positions[] = { 0.15f, 0.35f, 0.5f, 0.65f, 0.85f };

	std::vector< float > params = defaultParams();
	const unsigned int msId     = ChannelParam( 0, CC_MARK_SPACE );

	std::printf( "  channel 1 at its defaults (10k, 4.7k, 100 nF), Mark-Space from 0.15 to 0.85\n\n" );
	std::printf( "  %10s | %10s %8s | %7s\n", "Mark-Space", "period", "vs 0.5", "duty" );

	int failures = 0;
	double reference = 0.0;
	double lastDuty  = -1.0;
	for( float pos : positions )
	{
		params[ msId ] = pos;
		BenchParams bp = Resolve( params.data(), 0.0f ).bench;
		for( int i = 1; i < kChannels; ++i )
			bp.channel[ i ].reset = false;
		bp.patch.xSource = 1;
		bp.patch.ySource = 0;
		bp.patch.zSource = 0;
		bp.patch.xGain   = 1.0f;
		bp.patch.xOffset = 0.0f;
		bp.yoke.coilX    = 0.0;
		bp.yoke.deflectionGain = 1.0f;
		bp.yoke.rail     = 3.0f;

		Bench bench;
		bench.Prepare();
		bench.SetParams( bp );

		std::vector< double > t, x;
		runBench( bench, 40.0 * bp.channel[ 0 ].nominalPeriod + 0.05, t, x );

		double period = 0.0, duty = 0.0;
		int cycles = 0;
		if( !measureCycles( findEdges( t, x ), 5, period, duty, cycles ) )
		{
			std::fprintf( stderr, "markspace: no cycles at %.2f\n", static_cast< double >( pos ) );
			++failures;
			continue;
		}
		if( pos == 0.5f )
			reference = period;
		std::printf( "  %10.2f | %10.4g %+7.3f%% | %6.2f%%\n", static_cast< double >( pos ), period,
		             reference > 0.0 ? ( period / reference - 1.0 ) * 100.0 : 0.0, duty * 100.0 );
		if( duty <= lastDuty )
		{
			std::fprintf( stderr, "markspace: duty did not rise from %.2f to %.2f\n", lastDuty, duty );
			++failures;
		}
		lastDuty = duty;
	}

	//Every row against the centre, which is the stock circuit.
	{
		params[ msId ] = 0.5f;
		for( float pos : positions )
		{
			params[ msId ] = pos;
			const ChannelSpec& ch = Resolve( params.data(), 0.0f ).bench.channel[ 0 ];
			const double period   = 0.6931471805599453 * ( ch.rCharge + ch.rDischarge ) * ch.c;
			const double err      = ( period / ch.nominalPeriod - 1.0 ) * 100.0;
			if( std::fabs( err ) > 0.5 )
			{
				std::fprintf( stderr, "markspace: the resolved period at %.2f is %.3f%% off the stock one\n",
				              static_cast< double >( pos ), err );
				++failures;
			}
		}
	}

	std::printf( failures == 0 ? "markspace: ok\n" : "markspace: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --yoke
//---------------------------------------------------------------------------
//
// The coil is L/R: a first-order lag on the deflection current. Channel 1 is
// slowed to 7 Hz so its output sits on each rail for tens of milliseconds, a
// 2 ms coil is put on X, and the rise after an edge is fitted: ln(rail - x)
// against t is a straight line of slope -1/tau for a true exponential, and
// its intercept gives the instant the step happened, which the sample
// stream does not otherwise know to better than a sample.
int checkYoke()
{
	constexpr double kTau = 2.0e-3;

	BenchParams bp = benchWith( 100e3, 47e3, 1e-6 );//7.4 Hz, 134 ms period
	bp.yoke.coilX  = kTau;
	Bench bench;
	bench.Prepare();
	bench.SetParams( bp );

	std::vector< double > t, x;
	runBench( bench, 0.6, t, x );

	const Edges edges = findEdges( t, x );
	if( edges.rises.size() < 3 )
	{
		std::fprintf( stderr, "yoke: no rising edges found\n" );
		return 1;
	}

	//The last rising crossing with a full high phase after it. NOTE this is
	//the MIDPOINT crossing, which a first-order rise reaches tau*ln2 AFTER the
	//step -- not the step itself. Everything below is careful about that
	//distinction; an earlier version of this check was not, read its baseline
	//from a window that already contained the first 0.4 ms of the rise, and
	//reported a 2.2% error that was entirely its own.
	double edgeTime = -1.0;
	for( auto it = edges.rises.rbegin(); it != edges.rises.rend(); ++it )
		if( *it + 0.03 < t.back() )
		{
			edgeTime = *it;
			break;
		}
	if( edgeTime < 0.0 )
	{
		std::fprintf( stderr, "yoke: no rising edge with a plateau after it\n" );
		return 1;
	}

	const double dt = t.size() > 1 ? t[ 1 ] - t[ 0 ] : 0.0;
	if( !( dt > 0.0 ) )
	{
		std::fprintf( stderr, "yoke: the sample stream has no time step\n" );
		return 1;
	}

	//----------------------------------------------------------------------
	// tau, from the recurrence rather than from a fitted asymptote.
	//
	// A first-order lag sampled uniformly satisfies x[i+1] = a x[i] + b
	// exactly, with a = exp(-dt/tau). So regressing each sample against the
	// one before it over the rise recovers tau from the SLOPE alone, and the
	// asymptote -- which appears only in the intercept -- never enters the
	// measurement.
	//
	// That matters here, and it is the reason this check is not the obvious
	// ln(rail - x) fit. The drive into the coil is not a clean step: it comes
	// through the deflection amplifier's coupling capacitor, whose 2 s time
	// constant droops the "rail" by about 1.5% of the swing across the window
	// any asymptote would have to be measured in. Fitting against that
	// drooping level pulls tau 1.7% low -- consistently, in one direction, and
	// for a reason that has nothing to do with the coil. The recurrence is
	// immune to it: a slowly moving offset shifts b and leaves a alone.
	//----------------------------------------------------------------------
	double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
	int n = 0;
	for( size_t i = 1; i < t.size(); ++i )
	{
		//From the crossing itself out to six tau: past the step, and inside
		//the high phase (102 ms) by a wide margin.
		if( t[ i - 1 ] < edgeTime || t[ i ] > edgeTime + 6.0 * kTau )
			continue;
		sx += x[ i - 1 ];
		sy += x[ i ];
		sxx += x[ i - 1 ] * x[ i - 1 ];
		sxy += x[ i - 1 ] * x[ i ];
		++n;
	}
	if( n < 8 )
	{
		std::fprintf( stderr, "yoke: only %d samples on the rise\n", n );
		return 1;
	}

	const double alpha = ( n * sxy - sx * sy ) / ( n * sxx - sx * sx );
	const double beta  = ( sy - alpha * sx ) / n;
	if( !( alpha > 0.0 && alpha < 1.0 ) )
	{
		std::fprintf( stderr, "yoke: the recurrence slope is %g, which is not a decay\n", alpha );
		return 1;
	}
	const double tauFit    = -dt / std::log( alpha );
	const double asymptote = beta / ( 1.0 - alpha );

	//----------------------------------------------------------------------
	// And the direct reading, which is what the claim actually says: 63% at
	// one tau. The step is the last sample before the trace starts climbing,
	// found rather than assumed -- the drive changes on a sample boundary, so
	// there is an exact one to find.
	//----------------------------------------------------------------------
	//The biggest one-sample increment in the four tau before the crossing: a
	//lag's response is fastest at the instant it starts, so the sample before
	//that increment is the last one at the old level. Found this way and not
	//as "the first sample that rises", because the low plateau is NOT flat --
	//the coupling capacitor is drifting across it too, so there is no sample
	//there whose predecessor fell.
	size_t stepIndex  = 0;
	double biggestRise = 0.0;
	for( size_t i = 1; i < t.size(); ++i )
	{
		if( t[ i ] < edgeTime - 4.0 * kTau || t[ i ] > edgeTime )
			continue;
		const double rise = x[ i ] - x[ i - 1 ];
		if( rise > biggestRise )
		{
			biggestRise = rise;
			stepIndex   = i - 1;
		}
	}
	if( stepIndex == 0 )
	{
		std::fprintf( stderr, "yoke: could not find the instant the drive stepped\n" );
		return 1;
	}
	const double floorLevel = x[ stepIndex ];
	const double stepTime   = t[ stepIndex ];

	double atTau = 0.0, best = 1e9;
	for( size_t i = 0; i < t.size(); ++i )
	{
		const double d = std::fabs( t[ i ] - ( stepTime + kTau ) );
		if( d < best )
		{
			best  = d;
			atTau = ( x[ i ] - floorLevel ) / ( asymptote - floorLevel );
		}
	}

	int failures        = 0;
	const double tauErr = ( tauFit / kTau - 1.0 ) * 100.0;

	std::printf( "  a %.1f ms coil on X, channel 1 stepping at %.1f Hz, dt = %.1f us (%.0f samples a tau)\n",
	             kTau * 1e3, 1.0 / bp.channel[ 0 ].nominalPeriod, dt * 1e6, kTau / dt );
	std::printf( "  %d samples of the rise, regressed as x[i+1] = a x[i] + b: a = %.6f\n", n, alpha );
	std::printf( "  tau = -dt / ln a = %.4f ms (%+.3f%%, tolerance 1%%)\n", tauFit * 1e3, tauErr );
	std::printf( "  the drive steps at t = %.4f s; one tau later the trace is at %.2f%% of the way\n"
	             "  to the asymptote the recurrence implies (1 - 1/e is 63.21%%)\n", stepTime, atTau * 100.0 );

	if( std::fabs( tauErr ) > 1.0 )
	{
		std::fprintf( stderr, "yoke: fitted tau is %.3f%% off, tolerance 1%%\n", tauErr );
		++failures;
	}
	if( std::fabs( atTau - ( 1.0 - std::exp( -1.0 ) ) ) > 0.01 )
	{
		std::fprintf( stderr, "yoke: %.2f%% at tau, expected 63.21%% within a point\n", atTau * 100.0 );
		++failures;
	}

	std::printf( failures == 0 ? "yoke: ok\n" : "yoke: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --dots
//---------------------------------------------------------------------------
//
// Two squares into X and Y with no coil and no filter: the beam sits at one of
// four corners and crosses between them in a single interval. The renderer
// spreads one interval's energy along each crossing, so the crossings are
// faint lines and the corners hold nearly everything. This is the first thing
// the video shows and the cheapest demonstration that brightness is dwell.
//
// ## Why this accumulates over a beat period rather than measuring one frame
//
// One frame is 16.7 ms and the timers run at about 744 Hz, so a frame is
// twelve cycles during which the two channels' phase relationship barely
// moves. Whether the beam ever reaches a given corner in that frame depends
// on whether the two outputs' low phases happen to overlap at that phase
// offset -- and at 75.8% duty they usually do not. Measured over one frame
// this check found THREE dots, with the fourth at exactly zero, which is the
// model being right rather than wrong.
//
// The fourth corner arrives as the phases precess: the channels differ by 1%
// of a capacitor, so they beat at about 7 Hz and walk through every phase
// relationship in 135 ms. That crawl is the charm of the whole plugin, and it
// is what a viewer sees as four dots. So the measurement covers a full beat
// period -- accumulated on the CPU rather than left to the phosphor, so the
// result does not depend on the persistence setting.
int checkDots()
{
	constexpr float kSigma = 0.006f;
	constexpr float kPower = 0.10f;

	const int size = benchSize( kSigma );

	BenchParams bp = Resolve( defaultParams().data(), 0.0f ).bench;
	//As shipped: X = Ch1 Output, Y = Ch2 Output, Coil 0, no filter. Asserted
	//rather than assumed, so a changed default fails here and not silently.
	if( bp.patch.xSource != 1 || bp.patch.ySource != 2 || bp.yoke.coilX != 0.0 || bp.yoke.coilY != 0.0
	    || bp.channel[ 0 ].filterTau != 0.0 || bp.channel[ 1 ].filterTau != 0.0 )
	{
		std::fprintf( stderr, "dots: the shipped defaults are no longer two squares into an electrostatic X/Y\n" );
		return 1;
	}
	//Unity into the bench renderer, so the dots land at the outputs' own swing.
	//A television's overscan is not under test here.
	bp.yoke.deflectionGain = 1.0f;

	const double f1   = 1.0 / bp.channel[ 0 ].nominalPeriod;
	const double f2   = 1.0 / bp.channel[ 1 ].nominalPeriod;
	const double beat = std::fabs( f1 - f2 );
	const int frames  = beat > 0.0 ? static_cast< int >( std::ceil( 1.0 / beat / kFrameSeconds ) ) + 2 : 12;

	Bench bench;
	bench.Prepare();
	bench.SetParams( bp );

	BeamGeometry beam;
	if( !beam.InitGL() )
	{
		std::fprintf( stderr, "dots: the beam renderer would not initialise\n" );
		return 1;
	}
	Target target             = makeTarget( size, size, true );
	const GLint viewport[ 4 ] = { 0, 0, size, size };

	const int n = std::clamp( static_cast< int >( std::lround( kFrameSeconds * bench.SampleRate() ) ), 2, kMaxBlock );

	//Let the coupling capacitors settle before anything is measured: they have
	//a 2 s time constant and the figure drifts while they charge.
	for( int frame = 0; frame < 240; ++frame )
		bench.Render( n, kFrameSeconds );

	const double saturation = phosphor( kBenchPhosphor ).saturation;
	std::vector< double > excitation( static_cast< size_t >( size ) * size, 0.0 );
	int failures = 0;

	for( int frame = 0; frame < frames; ++frame )
	{
		const Sample* samples = bench.Render( n, kFrameSeconds );
		//clearHistory every frame, so what is summed here is what THIS frame
		//deposited and the phosphor's own persistence is out of the question.
		if( !beam.Render( samples, n, benchParams( kSigma, kPower ), target.fbo, viewport, 0, 1.0f, 1.0f ) )
		{
			std::fprintf( stderr, "dots: render failed on frame %d\n", frame );
			releaseTarget( target );
			beam.DeInitGL();
			return 1;
		}
		const std::vector< float > pixels = readFloats( target );
		for( size_t i = 0; i < excitation.size(); ++i )
			excitation[ i ] += excitationFrom( pixels[ i * 4 + 1 ], saturation );
	}

	releaseTarget( target );
	beam.DeInitGL();

	std::vector< double > cols( static_cast< size_t >( size ), 0.0 ), rows( static_cast< size_t >( size ), 0.0 );
	double total = 0.0;
	for( int y = 0; y < size; ++y )
		for( int x = 0; x < size; ++x )
		{
			const double e = excitation[ static_cast< size_t >( y ) * size + x ];
			cols[ static_cast< size_t >( x ) ] += e;
			rows[ static_cast< size_t >( y ) ] += e;
			total += e;
		}

	auto twoPeaks = [ & ]( const std::vector< double >& m, int& a, int& b ) {
		a = static_cast< int >( std::max_element( m.begin(), m.end() ) - m.begin() );
		const int guard = static_cast< int >( 20.0 * kSigma * 0.5 * size );
		b           = -1;
		double best = -1.0;
		for( int i = 0; i < size; ++i )
			if( std::abs( i - a ) > guard && m[ static_cast< size_t >( i ) ] > best )
			{
				best = m[ static_cast< size_t >( i ) ];
				b    = i;
			}
	};
	int cx0, cx1, cy0, cy1;
	twoPeaks( cols, cx0, cx1 );
	twoPeaks( rows, cy0, cy1 );

	//8 sigma each way is 99.99% of a spot plus the crossings' ends.
	const int radius = static_cast< int >( std::ceil( 8.0 * kSigma * 0.5 * size ) );
	auto window = [ & ]( int cx, int cy ) {
		double sum = 0.0;
		for( int y = std::max( 0, cy - radius ); y <= std::min( size - 1, cy + radius ); ++y )
			for( int x = std::max( 0, cx - radius ); x <= std::min( size - 1, cx + radius ); ++x )
				sum += excitation[ static_cast< size_t >( y ) * size + x ];
		return sum;
	};
	const double spots[ 4 ] = { window( cx0, cy0 ), window( cx1, cy0 ), window( cx0, cy1 ), window( cx1, cy1 ) };
	double inSpots = 0.0, smallest = 1e300;
	for( double s : spots )
	{
		inSpots += s;
		smallest = std::min( smallest, s );
	}
	const double fraction = total > 0.0 ? inSpots / total : 0.0;

	std::printf( "  channels at %.1f and %.1f Hz -- a %.1f Hz beat, so %d frames is one full precession\n",
	             f1, f2, beat, frames );
	std::printf( "  %d samples a frame at %.0f Hz; face %dx%d; spots at columns %d,%d and rows %d,%d\n",
	             n, bench.SampleRate(), size, size, cx0, cx1, cy0, cy1 );
	std::printf( "  light in the four %d-px windows: %.2f%% %.2f%% %.2f%% %.2f%% of the frame\n", 2 * radius + 1,
	             spots[ 0 ] / total * 100.0, spots[ 1 ] / total * 100.0, spots[ 2 ] / total * 100.0,
	             spots[ 3 ] / total * 100.0 );
	std::printf( "  together %.2f%% (tolerance >90%%); the rest is the crossings\n", fraction * 100.0 );

	if( !( total > 0.0 ) )
	{
		std::fprintf( stderr, "dots: nothing was drawn\n" );
		++failures;
	}
	else
	{
		if( fraction < 0.90 )
		{
			std::fprintf( stderr, "dots: only %.2f%% of the light is in the four spots\n", fraction * 100.0 );
			++failures;
		}
		//Every corner has to be visited. The faintest is the "both low" one --
		//at 75.8% duty the two low phases coincide for a small part of the
		//beat -- so the floor is 1%, not a quarter.
		if( smallest / total < 0.01 )
		{
			std::fprintf( stderr, "dots: the faintest corner holds %.3f%% -- that is not four dots\n",
			              smallest / total * 100.0 );
			++failures;
		}
	}

	std::printf( failures == 0 ? "dots: ok\n" : "dots: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --energy
//---------------------------------------------------------------------------
//
// vectrix's check, on vectrix's renderer: one straight line walked between
// once and a hundred times inside a single frame deposits the same light.
int checkEnergy()
{
	constexpr float kSigma = 0.006f;
	constexpr float kPower = 0.10f;
	constexpr double kSpan = 0.8;
	constexpr int kSamples = 4096;
	constexpr int kSpeeds  = 10;

	const int size = benchSize( kSigma );

	BeamGeometry beam;
	if( !beam.InitGL() )
	{
		std::fprintf( stderr, "energy: the beam renderer would not initialise\n" );
		return 1;
	}

	Target target             = makeTarget( size, size, true );
	const GLint viewport[ 4 ] = { 0, 0, size, size };

	std::vector< Sample > block( kSamples );
	std::vector< double > totals( kSpeeds, 0.0 );
	int failures = 0;

	for( int k = 0; k < kSpeeds; ++k )
	{
		const double s  = std::pow( 100.0, static_cast< double >( k ) / static_cast< double >( kSpeeds - 1 ) );
		const double dt = kFrameSeconds / static_cast< double >( kSamples - 1 );
		for( int i = 0; i < kSamples; ++i )
		{
			const double u    = static_cast< double >( i ) / static_cast< double >( kSamples - 1 );
			const double turn = u * s;
			const double frac = turn - std::floor( turn );
			block[ static_cast< size_t >( i ) ].x  = static_cast< float >( kSpan * ( 4.0 * std::fabs( frac - 0.5 ) - 1.0 ) );
			block[ static_cast< size_t >( i ) ].y  = 0.0f;
			block[ static_cast< size_t >( i ) ].z  = 1.0f;
			block[ static_cast< size_t >( i ) ].dt = static_cast< float >( dt );
		}

		if( !beam.Render( block.data(), kSamples, benchParams( kSigma, kPower ), target.fbo, viewport, 0, 1.0f, 1.0f ) )
		{
			std::fprintf( stderr, "  %.1f traversals: render failed\n", s );
			++failures;
			continue;
		}
		totals[ static_cast< size_t >( k ) ] = totalLight( readFloats( target ) );
	}

	releaseTarget( target );
	beam.DeInitGL();

	double lowest = 1.0e300, highest = 0.0, mean = 0.0;
	for( double total : totals )
	{
		lowest  = std::min( lowest, total );
		highest = std::max( highest, total );
		mean += total / static_cast< double >( kSpeeds );
	}
	if( !( mean > 0.0 ) )
	{
		std::fprintf( stderr, "energy: nothing was drawn at any speed\n" );
		return failures + 1;
	}
	const double spread = ( highest - lowest ) / mean * 100.0;

	std::printf( "  a %.3f-unit line, %d samples, one frame of beam-on time, sigma %g\n",
	             2.0 * kSpan, kSamples, static_cast< double >( kSigma ) );
	for( int k = 0; k < kSpeeds; ++k )
	{
		const double s = std::pow( 100.0, static_cast< double >( k ) / static_cast< double >( kSpeeds - 1 ) );
		std::printf( "  %7.2f traversals/frame   total light %.6e  %+.3f%%\n", s, totals[ static_cast< size_t >( k ) ],
		             ( totals[ static_cast< size_t >( k ) ] - mean ) / mean * 100.0 );
	}
	std::printf( "  spread %.4f%% of the mean (tolerance 0.5%%)\n", spread );

	if( spread > 0.5 )
	{
		std::fprintf( stderr, "energy: %.4f%% spread across 100:1 of speed, tolerance 0.5%%\n", spread );
		++failures;
	}

	std::printf( failures == 0 ? "energy: ok\n" : "energy: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --defaults
//---------------------------------------------------------------------------
int checkDefaults()
{
	AstablePlugin plugin;
	int failures = 0;
	for( int j = 0; j < kPresetParamCount; ++j )
	{
		const unsigned int id = PresetParamId( j );
		const float want      = presets::kPresets[ 0 ].v[ static_cast< size_t >( j ) ];
		const float got       = plugin.GetFloatParameter( id );
		if( want != got )
		{
			std::printf( "  %-16s row 1 says %g, the constructor says %g\n", plugin.GetParamName( id ),
			             static_cast< double >( want ), static_cast< double >( got ) );
			++failures;
		}
	}
	//----------------------------------------------------------------------
	// And every column holds a value of the right KIND.
	//
	// A standard column is 0..1, because SetParamInfo clamps a STANDARD
	// default into that range; an option column holds its element VALUE, which
	// is an integer index into that dropdown. Write 0.2 into an option and it
	// does not mean "a fifth of the way up" -- it rounds to 0 and the row
	// silently selects the first entry. That is the defect check_presets.py
	// exists for across the fleet, and the one that made vectrix's Star preset
	// draw a three-pointed star.
	//
	// The element count comes from Controls.h and NOT from GetParamRange:
	// FFGL reports an option's range as 0..1 whatever its element count is, so
	// a check written against the range passes every wrong value and fails
	// every right one. That cost a confused minute here.
	//----------------------------------------------------------------------
	auto elementsOf = []( unsigned int id ) -> int {
		if( id <= PT_CHANNEL_LAST )
		{
			const ChannelControl ctl = static_cast< ChannelControl >( ( id - PT_CHANNEL_FIRST ) % CC_COUNT );
			if( ctl == CC_C_DECADE )
				return kDecadeCount;
			if( ctl == CC_CV_SOURCE )
				return kCvSourceCount;
			if( ctl == CC_RESET )
				return 2;
			return 0;//standard: 0..1
		}
		if( id == PT_X_SOURCE || id == PT_Y_SOURCE || id == PT_Z_SOURCE )
			return kPatchSourceCount;
		if( id == PT_Z_MODE )
			return kZModeCount;
		return 0;
	};

	for( int r = 0; r < presets::kCount; ++r )
	{
		for( int j = 0; j < kPresetParamCount; ++j )
		{
			const unsigned int id = PresetParamId( j );
			const float v         = presets::kPresets[ r ].v[ static_cast< size_t >( j ) ];
			const int elements    = elementsOf( id );

			if( elements > 0 )
			{
				if( v != std::round( v ) || v < 0.0f || v > static_cast< float >( elements - 1 ) )
				{
					std::printf( "  %-16s %-16s = %g is not a whole number in 0..%d\n", presets::kPresets[ r ].name,
					             plugin.GetParamName( id ), static_cast< double >( v ), elements - 1 );
					++failures;
				}
			}
			else if( v < 0.0f || v > 1.0f )
			{
				std::printf( "  %-16s %-16s = %g is outside 0..1\n", presets::kPresets[ r ].name,
				             plugin.GetParamName( id ), static_cast< double >( v ) );
				++failures;
			}
		}
	}

	std::printf( "  %d covered parameters, %d preset rows\n", kPresetParamCount, presets::kCount );
	std::printf( failures == 0 ? "defaults: ok\n" : "defaults: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --presets
//---------------------------------------------------------------------------
int checkPresets()
{
	constexpr int kWidth = 640, kHeight = 360, kFrames = 12;
	Target target = makeTarget( kWidth, kHeight, false );

	std::vector< std::vector< unsigned char > > images;
	int failures = 0;

	for( int preset = 0; preset <= presets::kCount; ++preset )
	{
		AstablePlugin plugin;
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( preset ) );
		if( !startPlugin( plugin, target ) )
		{
			std::fprintf( stderr, "presets: InitGL failed for preset %d\n", preset );
			++failures;
			images.emplace_back();
			continue;
		}
		bool drew = true;
		for( int frame = 0; frame < kFrames && drew; ++frame )
			drew = renderFrame( plugin, target, frame );
		plugin.DeInitGL();
		if( !drew )
		{
			std::fprintf( stderr, "presets: render failed for preset %d\n", preset );
			++failures;
			images.emplace_back();
			continue;
		}
		images.push_back( readBytes( target ) );
	}
	releaseTarget( target );

	auto differ = []( const std::vector< unsigned char >& a, const std::vector< unsigned char >& b ) {
		long count = 0;
		for( size_t i = 0; i < a.size() && i < b.size(); ++i )
			if( a[ i ] != b[ i ] )
				++count;
		return count;
	};

	//Custom at the defaults and row 1 must render identically.
	if( images.size() > 1 && !images[ 0 ].empty() && !images[ 1 ].empty() )
	{
		const long d = differ( images[ 0 ], images[ 1 ] );
		std::printf( "  Custom vs %-16s %ld subpixels differ (must be 0)\n", presets::kPresets[ 0 ].name, d );
		if( d != 0 )
			++failures;
	}
	//Every other pair must differ.
	for( int a = 1; a <= presets::kCount; ++a )
		for( int b = a + 1; b <= presets::kCount; ++b )
		{
			if( images[ static_cast< size_t >( a ) ].empty() || images[ static_cast< size_t >( b ) ].empty() )
				continue;
			const long d = differ( images[ static_cast< size_t >( a ) ], images[ static_cast< size_t >( b ) ] );
			if( d == 0 )
			{
				std::printf( "  %s and %s render the same picture\n", presets::kPresets[ a - 1 ].name,
				             presets::kPresets[ b - 1 ].name );
				++failures;
			}
		}
	//And every preset lights something.
	for( int a = 1; a <= presets::kCount; ++a )
	{
		const auto& img = images[ static_cast< size_t >( a ) ];
		long lit = 0;
		for( size_t i = 0; i < img.size(); i += 4 )
			if( img[ i ] + img[ i + 1 ] + img[ i + 2 ] > 0 )
				++lit;
		std::printf( "  %-16s %ld lit pixels\n", presets::kPresets[ a - 1 ].name, lit );
		if( lit == 0 )
			++failures;
	}

	std::printf( failures == 0 ? "presets: ok\n" : "presets: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
/// FFGL truncates a parameter NAME at 16 characters, and the DISPLAY string
/// too: FF_GET_PARAMETER_DISPLAY hands the host a 16-byte buffer. Both happen
/// in the host, silently. A display is a function of the values, so it is
/// swept rather than read once.
int checkNames()
{
	AstablePlugin plugin;
	int over = 0;

	std::printf( "names and displays longer than FFGL's 16 characters:\n\n" );
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && std::strlen( name ) > 16 )
		{
			std::printf( "  name     %-3u  %-28s %zu\n", id, name, std::strlen( name ) );
			++over;
		}
	}

	std::vector< float > defaults( PT_COUNT );
	for( unsigned int id = 0; id < PT_COUNT; ++id )
		defaults[ id ] = plugin.GetFloatParameter( id );

	std::map< unsigned int, std::string > tooLong;
	for( unsigned int swept = 0; swept < PT_ABOUT_TEXT; ++swept )
	{
		if( plugin.GetParamType( swept ) != FF_TYPE_STANDARD )
			continue;
		for( int step = 0; step <= 50; ++step )
		{
			plugin.SetFloatParameter( swept, static_cast< float >( step ) / 50.0f );
			for( unsigned int id = 0; id < PT_ABOUT_TEXT; ++id )
			{
				const char* display = plugin.GetParameterDisplay( id );
				if( display != nullptr && std::strlen( display ) > 16 && std::strlen( display ) > tooLong[ id ].size() )
					tooLong[ id ] = display;
			}
		}
		plugin.SetFloatParameter( swept, defaults[ swept ] );
	}
	for( const auto& entry : tooLong )
	{
		std::printf( "  display  %-3u  %-18s %-24s %zu\n", entry.first, plugin.GetParamName( entry.first ),
		             entry.second.c_str(), entry.second.size() );
	}
	over += static_cast< int >( tooLong.size() );

	std::printf( "\n  %d over the limit\n", over );
	std::printf( over == 0 ? "names: ok\n" : "names: FAILED\n" );
	return over == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
int runBenchmark()
{
	const struct
	{
		int w, h;
		const char* label;
	} sizes[] = { { 1280, 720, "1280x720" }, { 1920, 1080, "1920x1080" }, { 3840, 2160, "3840x2160" } };

	std::printf( "  ms/frame, 120 frames after 20 of warm-up, glFinish on both sides, shipped defaults\n\n" );
	for( const auto& sz : sizes )
	{
		Target target = makeTarget( sz.w, sz.h, false );
		AstablePlugin plugin;
		if( !startPlugin( plugin, target ) )
		{
			std::fprintf( stderr, "bench: InitGL failed\n" );
			releaseTarget( target );
			return 1;
		}
		int frame = 0;
		for( ; frame < 20; ++frame )
			renderFrame( plugin, target, frame );
		glFinish();
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < 120; ++i, ++frame )
			renderFrame( plugin, target, frame );
		glFinish();
		const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / 120.0;
		std::printf( "  %-10s %7.3f ms/frame  %5.1f%% of a 60 fps frame\n", sz.label, ms, ms / ( 1000.0 / 60.0 ) * 100.0 );
		plugin.DeInitGL();
		releaseTarget( target );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters()
{
	AstablePlugin plugin;

	const char* types[ 256 ] = {};
	types[ FF_TYPE_BOOLEAN ]  = "boolean";
	types[ FF_TYPE_EVENT ]    = "event";
	types[ FF_TYPE_STANDARD ] = "standard";
	types[ FF_TYPE_OPTION ]   = "option";
	types[ FF_TYPE_BUFFER ]   = "buffer";
	types[ FF_TYPE_INTEGER ]  = "integer";
	types[ FF_TYPE_TEXT ]     = "text";

	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name        = plugin.GetParamName( id );
		const unsigned int type = plugin.GetParamType( id );
		const char* typeName    = ( type < 256 && types[ type ] != nullptr ) ? types[ type ] : "other";

		std::printf( "%3u  %-24s %-9s %10.4f", id, name != nullptr ? name : "?", typeName,
		             plugin.GetFloatParameter( id ) );

		const RangeStruct range = plugin.GetParamRange( id );
		if( range.min != range.max )
			std::printf( "   [%g .. %g]", range.min, range.max );

		const char* shown = plugin.GetParameterDisplay( id );
		if( shown != nullptr && *shown != '\0' )
			std::printf( "   %s", shown );
		std::printf( "\n" );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --pipe: raw frames out, and the cue sheet that automates them.
//
// The format is the fleet's -- tinsel's tinseltest, porthole's phtest,
// old-cathode's octest -- on purpose, so one build.py can film any of them.
// The difference here is the one that matters about this plugin: it is a
// SOURCE. `SetMinInputs( 0 )`, `SetMaxInputs( 0 )`, `numInputTextures = 0`,
// `HasClip` 0 in the glass shader. So this end of the pipe has no stdin side
// at all, and the loop is bounded by a frame count or by the reader hanging up
// rather than by end of input.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

/// One `frame  Parameter Name  value` per line. `frame  Name=value` is accepted
/// too, because `--set` spells it that way and a filming script that mixes the
/// two should not be a silent no-op. `#` starts a comment.
std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them ("Ch1 Mark-Space") and a value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );

		if( words.empty() )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		std::string name;
		float value = 0.0f;

		const size_t equals = words.back().find( '=' );
		if( words.size() == 1 || equals != std::string::npos )
		{
			//`Name=value`, possibly with the name's own spaces ahead of it.
			if( equals == std::string::npos )
			{
				error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
				return {};
			}
			value = std::strtof( words.back().substr( equals + 1 ).c_str(), nullptr );
			words.back().erase( equals );
			for( const std::string& part : words )
			{
				if( part.empty() )
					continue;
				name += name.empty() ? part : " " + part;
			}
		}
		else
		{
			value = std::strtof( words.back().c_str(), nullptr );
			words.pop_back();
			for( const std::string& part : words )
				name += name.empty() ? part : " " + part;
		}

		if( name.empty() )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

struct PipeOptions
{
	int width   = 1920;
	int height  = 1080;
	int frames  = 0;   ///< 0 means "until the reader hangs up".
	double fps  = 60.0;
	int preset  = 0;
	float audio = -1.0f;
	std::string scriptPath;
	std::vector< std::pair< std::string, float > > sets;
};

int runPipe( const PipeOptions& options )
{
	//A reader that stops early -- `head -c`, ffmpeg hitting its own -t, a
	//pipeline the operator interrupted -- would otherwise kill this process
	//with SIGPIPE before it could shut the plugin down. Ignored, so the write
	//returns EPIPE and the loop ends the ordinary way.
	std::signal( SIGPIPE, SIG_IGN );

	AstablePlugin plugin;
	if( options.preset > 0 )
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( options.preset ) );
	int failures = applySets( plugin, options.sets );
	if( options.audio >= 0.0f )
		plugin.SetAudioForTest( options.audio );
	if( failures > 0 )
		return failures;

	//Resolve the script's names to ids once, up front, and refuse a name that
	//is not a parameter.
	std::map< unsigned int, Track > automation;
	if( !options.scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( options.scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "attest: %s\n", error.c_str() );
			return 1;
		}

		const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
		for( const auto& entry : tracks )
		{
			const auto found = byName.find( entry.first );
			if( found == byName.end() )
			{
				std::fprintf( stderr, "attest: the script names \"%s\", which is not a parameter (try --list)\n",
				              entry.first.c_str() );
				return 1;
			}
			automation[ found->second ] = entry.second;
		}
	}

	Target target = makeTarget( options.width, options.height, false );
	if( !startPlugin( plugin, target ) )
	{
		std::fprintf( stderr, "attest: InitGL failed\n" );
		releaseTarget( target );
		return 1;
	}

	const double frameSeconds = options.fps > 0.0 ? 1.0 / options.fps : kFrameSeconds;

	for( int index = 0; options.frames <= 0 || index < options.frames; ++index )
	{
		for( const auto& track : automation )
			plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

		if( !renderFrame( plugin, target, index, frameSeconds ) )
		{
			std::fprintf( stderr, "attest: ProcessOpenGL failed on frame %d\n", index );
			++failures;
			break;
		}

		//Top row first on the way out, because that is what a raw RGBA stream
		//is and GL hands back bottom row first.
		const std::vector< unsigned char > out = flipRows( readBytes( target ), options.width, options.height );

		size_t written = 0;
		bool hungUp    = false;
		while( written < out.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
			if( put <= 0 )
			{
				hungUp = true;
				break;
			}
			written += static_cast< size_t >( put );
		}
		if( hungUp )
			break;
	}

	plugin.DeInitGL();
	releaseTarget( target );
	return failures;
}

void usage()
{
	std::printf(
		"attest -- the astable offline harness\n"
		"\n"
		"  --period            period = 0.693 (Ra + 2Rb) C, duty = (Ra + Rb) / (Ra + 2Rb), within 1%%\n"
		"  --swing             the capacitor runs between V5/2 and V5 within 1%%\n"
		"  --recover           a channel driven on pin 5 comes back when the CV is removed\n"
		"  --markspace         Mark-Space moves the duty and holds the period within 0.5%%\n"
		"  --dots              two squares into X and Y: >90%% of the light in four spots\n"
		"  --yoke              a step into a coil is an exponential of the right tau\n"
		"  --energy            total light is independent of sweep speed within 0.5%%\n"
		"  --presets           every preset renders, all distinct, row 1 = Custom\n"
		"  --defaults          preset row 1 IS the constructor's defaults (no GL)\n"
		"  --names             names and displays fit FFGL's 16 characters (no GL)\n"
		"  --bench             ms/frame at 720p, 1080p and 4K\n"
		"  --all               every check above, with a summary\n"
		"\n"
		"  --out PATH          render one frame to a PNG\n"
		"  --size WxH          the raster (default 1920x1080)\n"
		"  --width N           the raster's width, as an alternative to --size\n"
		"  --height N          the raster's height\n"
		"  --frames N          frames to render before capturing the last (default 8);\n"
		"                      with --pipe, how many to write (default: until the reader stops)\n"
		"  --preset N          apply factory preset N (1 .. %d)\n"
		"  --set NAME=VALUE    set a parameter, by id or by name (repeatable)\n"
		"  --audio L           a flat spectrum whose folded level is L (0..1)\n"
		"  --list              every parameter: id, name, type, current value, range, display\n"
		"\n"
		"  --pipe              raw RGBA frames on stdout. A SOURCE reads nothing, so there is\n"
		"                      no stdin side:  attest --pipe --width 1920 --height 1080 |  ffmpeg ...\n"
		"  --fps N             the synthetic frame rate --pipe advances at (default 60)\n"
		"  --script PATH       parameter cues for --pipe: `frame  Parameter Name  value`\n",
		presets::kCount );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath;
	std::vector< std::pair< std::string, float > > sets;

	int width  = 1920;
	int height = 1080;
	int frames = 8;
	int preset = 0;
	float audio = -1.0f;
	double fps  = 60.0;
	std::string scriptPath;

	//`--frames` means two different things and only one of them has a sane
	//default. For a still it is "settle for this many"; for --pipe it is the
	//length of the take, and 8 frames of video is not a default anybody wants.
	//So an unasked-for --frames leaves --pipe running until its reader stops.
	bool framesGiven = false;

	bool doList = false, doNames = false, doDefaults = false;
	bool doPeriod = false, doSwing = false, doMarkSpace = false, doDots = false, doYoke = false;
	bool doRecover = false;
	bool doEnergy = false, doPresets = false, doBench = false, doPipe = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return ( i + 1 < argc ) ? argv[ ++i ] : std::string(); };

		if( arg == "--out" ) outPath = next();
		else if( arg == "--frames" ) { frames = std::atoi( next().c_str() ); framesGiven = true; }
		else if( arg == "--width" ) width = std::atoi( next().c_str() );
		else if( arg == "--height" ) height = std::atoi( next().c_str() );
		else if( arg == "--fps" ) fps = std::atof( next().c_str() );
		else if( arg == "--script" ) scriptPath = next();
		else if( arg == "--pipe" ) doPipe = true;
		else if( arg == "--preset" ) preset = std::atoi( next().c_str() );
		else if( arg == "--audio" ) audio = static_cast< float >( std::atof( next().c_str() ) );
		else if( arg == "--list" ) doList = true;
		else if( arg == "--names" ) doNames = true;
		else if( arg == "--defaults" ) doDefaults = true;
		else if( arg == "--period" ) doPeriod = true;
		else if( arg == "--swing" ) doSwing = true;
		else if( arg == "--recover" ) doRecover = true;
		else if( arg == "--markspace" ) doMarkSpace = true;
		else if( arg == "--dots" ) doDots = true;
		else if( arg == "--yoke" ) doYoke = true;
		else if( arg == "--energy" ) doEnergy = true;
		else if( arg == "--presets" ) doPresets = true;
		else if( arg == "--bench" ) doBench = true;
		else if( arg == "--all" )
			doNames = doDefaults = doPeriod = doSwing = doRecover = doMarkSpace = doDots = doYoke = doEnergy = doPresets = true;
		else if( arg == "--size" )
		{
			const std::string value = next();
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
		}
		else if( arg == "--set" )
		{
			const std::string value = next();
			const size_t equals     = value.find( '=' );
			if( equals != std::string::npos )
				sets.emplace_back( value.substr( 0, equals ),
				                   static_cast< float >( std::atof( value.substr( equals + 1 ).c_str() ) ) );
		}
		else
		{
			usage();
			return arg == "--help" || arg == "-h" ? 0 : 1;
		}
	}

	const bool anyGL = doDots || doEnergy || doPresets || doBench || doPipe || !outPath.empty();
	const bool any   = anyGL || doList || doNames || doDefaults || doPeriod || doSwing || doRecover || doMarkSpace || doYoke;
	if( !any )
	{
		usage();
		return 1;
	}

	if( doPipe && ( width <= 0 || height <= 0 ) )
	{
		std::fprintf( stderr, "attest: --pipe needs a positive --width and --height\n" );
		return 1;
	}

	//Line-buffered, so a failure on stderr appears where it happened.
	//
	//Except under --pipe, where stdout is the video. Buffering is left alone
	//there and nothing below prints to it -- a single line of "wrote ..." in
	//the middle of a raw RGBA stream is four hundred bytes of green noise
	//somewhere near the top of one frame, which is very hard to recognise for
	//what it is.
	if( !doPipe )
		std::setvbuf( stdout, nullptr, _IOLBF, 0 );

	int failures = 0;

	//No GL: these run before the context exists and work with no display.
	if( doList )
		failures += listParameters();
	if( doNames )
		failures += checkNames();
	if( doDefaults )
		failures += checkDefaults();
	if( doPeriod )
		failures += checkPeriod();
	if( doSwing )
		failures += checkSwing();
	if( doRecover )
		failures += checkRecover();
	if( doMarkSpace )
		failures += checkMarkSpace();
	if( doYoke )
		failures += checkYoke();

	CGLContextObj context = nullptr;
	if( anyGL )
	{
		context = createContext();
		if( context == nullptr )
		{
			std::fprintf( stderr, "could not create a GL 4.1 core context\n" );
			return 1;
		}
	}

	if( doEnergy )
		failures += checkEnergy();
	if( doDots )
		failures += checkDots();
	if( doPresets )
		failures += checkPresets();
	if( doBench )
		failures += runBenchmark();

	if( doPipe )
	{
		PipeOptions options;
		options.width      = width;
		options.height     = height;
		options.frames     = framesGiven ? frames : 0;
		options.fps        = fps;
		options.preset     = preset;
		options.audio      = audio;
		options.scriptPath = scriptPath;
		options.sets       = sets;
		failures += runPipe( options );
	}

	if( !outPath.empty() )
	{
		Target target = makeTarget( width, height, false );

		AstablePlugin plugin;
		if( preset > 0 )
			plugin.SetFloatParameter( PT_PRESET, static_cast< float >( preset ) );
		failures += applySets( plugin, sets );
		if( audio >= 0.0f )
			plugin.SetAudioForTest( audio );

		if( !startPlugin( plugin, target ) )
		{
			std::fprintf( stderr, "InitGL failed\n" );
			++failures;
		}
		else
		{
			bool drew = true;
			for( int frame = 0; frame < std::max( 1, frames ) && drew; ++frame )
				drew = renderFrame( plugin, target, frame );

			if( !drew )
			{
				std::fprintf( stderr, "render failed\n" );
				++failures;
			}
			else
			{
				const std::vector< unsigned char > image = flipRows( readBytes( target ), width, height );
				if( !writePng( outPath, width, height, image ) )
				{
					std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
					++failures;
				}
				else
					std::printf( "wrote %s (%d x %d, %d frames at 1/60 s, engine %.0f Hz)\n", outPath.c_str(), width,
					             height, std::max( 1, frames ), plugin.TheBench().SampleRate() );
			}
			plugin.DeInitGL();
		}
		releaseTarget( target );
	}

	if( doNames && doDefaults && doPeriod && doSwing && doRecover && doMarkSpace && doDots && doYoke && doEnergy && doPresets )
		std::printf( "\n%s\n", failures == 0 ? "all checks passed" : "SOME CHECKS FAILED" );

	if( context != nullptr )
	{
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
	}
	return failures == 0 ? 0 : 1;
}
