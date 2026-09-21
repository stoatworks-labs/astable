/**
 * Astable — browser demo.
 *
 * Six 555 timers, a patch bay and a television's deflection yoke. The one idea,
 * from `AGENTS.md`: **the picture is where the beam went.** Nothing on the face
 * is drawn as a shape. Two square waves into X and Y give four dots because the
 * beam dwells at the rails and crosses between them fast — and the crossings are
 * faint for the same reason, not because anything decided to draw them faintly.
 *
 * Two halves, and they are not equally faithful:
 *
 *   The renderer is the plugin's. The nine shader constants below are
 *   `kConstants`, `kFragmentHelpers`, `kScreenVertexBody`, `kTraceVertexBody`,
 *   `kTraceFragmentBody`, `kDecayFragmentBody`, `kBrightFragmentBody`,
 *   `kBlurFragmentBody` and `kGlassFragmentBody` from
 *   `source/render/shaders/`, copied across unedited, assembled the way
 *   `Prelude.cpp` assembles them, and run in the same six passes in the same
 *   order as `BeamGeometry::Render`. `demo/tools/check_shaders.py` compares them
 *   to the C++ character for character and `tools/verify.sh` runs it, because
 *   two copies of a shader is exactly the arrangement that drifts.
 *
 *   The engine is a port, and this time a complete one. `Timer555`, `Bench`,
 *   `Phosphor`, `Tube`, `Controls` and `Presets` are all here, all six channels,
 *   at the plugin's own engine rate. What is missing is listed below and on the
 *   page; none of it is the circuit.
 *
 * The rules the port keeps, exactly, because breaking one gives a picture that
 * looks plausible and is wrong:
 *
 *   - **`step` solves its comparator crossings, it does not sample them.** A
 *     sampled comparator is up to a sample late every cycle, always in the same
 *     direction — a 1% period error at 96 samples a period, which is the
 *     tolerance the plugin claims.
 *   - **The capacitor discharges toward ground**, not toward a saturated
 *     transistor's 0.1 V. The datasheet's 0.693 assumes ground.
 *   - **`dt` lives in the sample.** That is what makes brightness independent of
 *     the engine rate, here as there.
 *   - **Sample 0 of a block is the previous block's last state, un-stepped**, so
 *     the n-1 intervals the renderer draws cover exactly one frame.
 *   - **A parameter change does not reset the timers.** `setParams` runs every
 *     frame and touches no state.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **No clip picker, and no "use my own file".** Astable is an FFGL *source*:
 * `SetMinInputs( 0 )`, `SetMaxInputs( 0 )`, and the glass shader's `HasClip` is
 * 0 for the source build so `ClipTexture` is never read. The kit's transport
 * offers both controls to every demo; this page removes them from the DOM after
 * mounting rather than leaving them present and inert, because a control that is
 * there and does nothing is worse than one that is absent. `demo.blurb` replaces
 * the banner's middle clause for the same reason — the stock wording says the
 * page runs "on generated clips", which would be the banner itself making the
 * kind of claim the banner exists to prevent.
 *
 * **Nothing audio.** The plugin declares an `Audio` FFT buffer parameter that
 * Resolume fills, an `Audio Gain` control over it, and an `Audio` element in
 * every channel's CV Source dropdown. A browser has no Resolume FFT, and asking
 * a visitor for a microphone to demonstrate a video source is not a trade worth
 * making. So the buffer parameter and Audio Gain are **absent** from this panel,
 * and the `Audio` element stays in the CV Source list — it is the plugin's
 * element list and the value is stored as its index — reading as a permanent
 * silence. That is said in its hint and in the disclosure.
 *
 * **Preset is the plugin's control, not the kit's.** The kit offers a
 * `demo.presets` dropdown that writes values into the panel. The plugin's
 * `Preset` is something else: an OVERRIDE applied at read time, because Resolume
 * does not consume value events, so a plugin cannot push values back into the
 * inspector. While it is on anything but Custom the sliders show one thing and
 * the bench runs another. Reproducing that is worth more than papering over it,
 * so `Preset` is declared as an ordinary option parameter here and `effective()`
 * below is `AstablePlugin::Effective` — the whole factory table from
 * `source/Presets.h`, mirrored column for column. Row 1 is also the defaults, by
 * construction, which is why the page opens on Four Dots.
 *
 * **The About block is not a parameter here.** The plugin declares a text line
 * and three link buttons so a host has somewhere to put them. A browser page has
 * links of its own, at the top.
 *
 * **The renderer's fixed settings are fixed here too.** `spotDefocus`,
 * `halation`, `halationRadius`, `halationThreshold`, `curvature`, `vignette`,
 * `graticule`, `faceBlack` and `opacity` are constants in `Astable.cpp` —
 * vectrix exposes them, a portable television has no knobs for them — so they
 * are constants below, with the plugin's own values.
 *
 * ---------------------------------------------------------------------------
 * What this page is NOT evidence about
 * ---------------------------------------------------------------------------
 *
 * This is GLSL ES 3.00 in WebGL2, not desktop GL 4.1 core. The shader text is
 * the same, but the driver, the precision hints and the rounding are not, so a
 * pixel here is not a measurement of a pixel there. Nothing on this page
 * measures anything: the numerical proof — the period against
 * 0.693 (Ra + 2Rb) C, the capacitor's swing against pin 5, the mark/space pot
 * moving duty without moving period, the four dots holding 90% of the light,
 * brightness independent of beam speed — is `tools/attest` in the repository,
 * and that harness is the reason to believe the model.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/render/shaders/. Do not edit here.
//
// The backticks inside the comments are escaped, because a template literal has
// nowhere else to go; check_shaders.py decodes that one escape before comparing
// and rejects any other backslash, so the escape cannot hide a difference.
//---------------------------------------------------------------------------

const CONSTANTS = `#version 410 core

const float Extent      = 4.5;
const float Sqrt2Pi     = 2.50662827463100050;
const float InvSqrt2Pi  = 0.39894228040143268;
`;

const FRAGMENT_HELPERS = `
// The two-layer phosphor, as a table entry from Phosphor.cpp.
uniform vec3  FastColour;
uniform vec3  SlowColour;
uniform float PhosphorEfficiency;
uniform float PhosphorSaturation;

// The graticule. \`GraticuleLevel\` already carries both the operator's strength
// and the resolution fade -- see graticuleAt().
uniform float GraticuleLevel;
uniform float GraticuleDiv;   //beam units per division
uniform vec3  GraticuleColour;

//---------------------------------------------------------------------------
// The standard normal CDF.
//
// GLSL has no erf, so this is the usual tanh approximation, good to about 3e-4.
// The clamp is not tidiness. A driver computing tanh as (e^2x - 1)/(e^2x + 1)
// overflows to inf around x = 44 and then produces inf/inf = NaN -- long after
// the value has stopped changing, and for arguments a beam sitting still
// produces constantly. One NaN deposited into the phosphor buffer survives every
// ping-pong for the life of the plugin, because NaN * decay is NaN.
//---------------------------------------------------------------------------
float ncdf( float x )
{
	float t = clamp( 0.7978845608 * ( x + 0.044715 * x * x * x ), -8.0, 8.0 );
	return 0.5 * ( 1.0 + tanh( t ) );
}

//---------------------------------------------------------------------------
// Excitation -> light.
//
// A phosphor has a finite number of luminescent centres, so pumping it harder
// stops buying proportionally more light. l = e / (1 + e/S) is the simplest
// curve with that shape: at low e it is e to within e^2/S, so the dwell law
// stays exact everywhere it is visible, and it approaches S asymptotically
// rather than clipping.
//
// This is the *only* thing that bounds a stationary beam, and it is deliberately
// the tube that does it rather than a clamp. A clamp would put a hard edge on
// the spot at whatever level it was set to; a real over-driven spot blooms,
// because the centres near the middle saturate first and the ones further out
// are still climbing.
//---------------------------------------------------------------------------
float phosphorSaturate( float e )
{
	return e / ( 1.0 + e / max( PhosphorSaturation, 1e-6 ) );
}

vec3 phosphorEmission( vec2 excitation )
{
	vec2 e = max( excitation, vec2( 0.0 ) );
	return ( phosphorSaturate( e.x ) * FastColour
	       + phosphorSaturate( e.y ) * SlowColour ) * PhosphorEfficiency;
}

//---------------------------------------------------------------------------
// The graticule, as a signed-distance evaluation rather than geometry.
//
// 8 divisions by 10, minor ticks every 0.2 of a division on the two centre axes
// only -- which is where a real scope puts them, because they are for reading a
// timebase off the horizontal and an amplitude off the vertical, not for
// subdividing the whole face.
//
// \`face\` is in beam units and \`GraticuleDiv\` converts to divisions, so this
// function knows nothing about the output resolution and does not need to: the
// anti-aliasing comes from the local derivative, which is correct in whichever
// buffer the caller happens to be filling.
//
// The visibility fade is the exception and it arrives as a uniform. Once a
// division is worth fewer than about three output pixels the graticule stops
// being a graticule and becomes a moire generator, so it fades out -- but the
// bright pass runs at quarter resolution, and a fade computed from the local
// derivative would therefore fade the halo out two rungs before the sharp copy
// and leave a graticule casting no light. One number, computed once from the
// output size, keeps them agreeing.
//---------------------------------------------------------------------------
float graticuleAt( vec2 face )
{
	if( GraticuleLevel <= 0.0 )
		return 0.0;

	vec2 g = face / max( GraticuleDiv, 1e-6 );
	const vec2 halfSpan = vec2( 5.0, 4.0 );//10 wide by 8 tall

	//Per-axis, not the length of the pair: the two are independent here and
	//combining them would overstate the rate by root two on every diagonal.
	vec2 rate = max( fwidth( g ), vec2( 1e-6 ) );

	const float lineHalf = 0.015;//1.5% of a division
	const float tickHalf = 0.09; //how far a minor tick reaches off its axis

	vec2 toLine = abs( g - round( g ) );
	float major = max( 1.0 - smoothstep( lineHalf - rate.x, lineHalf + rate.x, toLine.x ),
	                   1.0 - smoothstep( lineHalf - rate.y, lineHalf + rate.y, toLine.y ) );

	vec2 toTick = abs( g - 0.2 * round( g * 5.0 ) );
	float tickX = ( 1.0 - smoothstep( lineHalf - rate.x, lineHalf + rate.x, toTick.x ) )
	            * ( 1.0 - smoothstep( tickHalf, tickHalf + rate.y, abs( g.y ) ) );
	float tickY = ( 1.0 - smoothstep( lineHalf - rate.y, lineHalf + rate.y, toTick.y ) )
	            * ( 1.0 - smoothstep( tickHalf, tickHalf + rate.x, abs( g.x ) ) );

	float ink = max( major, max( tickX, tickY ) );

	//Nothing outside the rectangle. The border lines themselves are at +-5 and
	//+-4, which round() already treats as division lines, so this only has to
	//cut off what lies beyond them.
	vec2 beyond = smoothstep( halfSpan + lineHalf - rate, halfSpan + lineHalf + rate, abs( g ) );
	ink *= ( 1.0 - beyond.x ) * ( 1.0 - beyond.y );

	return ink * GraticuleLevel;
}
`;

const SCREEN_VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const TRACE_VERTEX_BODY = `
layout( location = 0 ) in vec4 sampleA;//x, y volts; z grid; w dt seconds
layout( location = 1 ) in vec4 sampleB;

uniform float BeamPower;
uniform float SpotSigma;      //beam units; 1 unit = half the face height
uniform float SpotDefocus;    //how much a full-current beam swells
uniform float BlankFloor;     //what a fully cut-off beam still puts on the glass
uniform float DensityFloor;   //peak areal density below which a segment is not drawn
uniform float DeflectionGain; //beam units per volt: underscan or overscan
uniform float FaceAspect;     //face width / face height

flat out float segLength;
flat out float segSigma;
flat out float segEnergy;
out vec2 segUV;               //x along the segment from its centre, y across

//---------------------------------------------------------------------------
// Not isnan()/isinf().
//
// Those are the right functions and they are also the first thing a shader
// compiler running with fast-math assumptions folds to constant false, on the
// grounds that the value cannot happen. The value is exactly what we are here
// about: one NaN sample -- an oscillator that divided by a zero frequency, an
// audio file with a denormal run, a filter that went unstable for one block --
// deposits a NaN into the phosphor buffer, and NaN * decay is NaN for the rest
// of the session. A comparison chain has no intrinsic to fold.
//---------------------------------------------------------------------------
bool usable( float v )
{
	return v > -1e30 && v < 1e30;
}

bool usable2( vec2 v )
{
	return usable( v.x ) && usable( v.y );
}

void main()
{
	vec2 a = sampleA.xy * DeflectionGain;
	vec2 b = sampleB.xy * DeflectionGain;

	//The grid voltage over the interval, averaged across its two ends. The floor
	//is what a cut-off beam still manages: a real gun does not reach zero
	//emission, and a retrace that leaves absolutely nothing behind looks wrong in
	//a way people describe as "too clean".
	float zBar = mix( BlankFloor, 1.0, clamp( 0.5 * ( sampleA.z + sampleB.z ), 0.0, 1.0 ) );

	//The whole brightness model, in one line. Energy per interval, not intensity
	//per pixel: the fragment stage spreads this over however far the beam went,
	//so nothing anywhere divides by a speed. \`dt\` is per-sample precisely so this
	//is independent of how many samples the engine chose to emit -- see
	//signal/Signal.h.
	float energy = BeamPower * max( sampleA.w, 0.0 ) * zBar;

	//More current is a fatter spot. Same reason a CRT's highlights swell.
	float sigma = SpotSigma * ( 1.0 + SpotDefocus * zBar );

	vec2 delta = b - a;
	float span = length( delta );
	vec2 dir   = span > 1e-9 ? delta / span : vec2( 1.0, 0.0 );

	//A floor of a twentieth of a spot, so the box is never zero-area and the
	//fragment stage's short branch has a length to divide by if it ever wanted
	//one. It does not: below a quarter of a sigma it uses the point limit, where
	//the length cancels.
	float len = max( span, 0.05 * sigma );

	//Peak areal density of the finished segment: the value at the middle of a
	//long one, E / (L * sigma * sqrt(2pi)). Culling on this rather than on energy
	//is what makes the cull mean something -- a fast sweep and a slow one can
	//carry the same energy and only one of them is visible.
	float density = energy / max( len * sigma * Sqrt2Pi, 1e-30 );

	bool ok = usable2( a ) && usable2( b )
	       && usable( energy ) && usable( sigma )
	       && energy > 0.0 && sigma > 0.0
	       && density >= DensityFloor;

	if( !ok )
	{
		//A degenerate quad: the same clip position for all four corners, so it
		//has no area and rasterises nothing whatever the driver does with it,
		//and outside the frustum as well. Returning without writing gl_Position
		//would be undefined; discarding in the fragment stage would be too late,
		//because a NaN position can already have taken the primitive somewhere
		//enormous.
		segLength   = 0.0;
		segSigma    = 1.0;
		segEnergy   = 0.0;
		segUV       = vec2( 0.0 );
		gl_Position = vec4( 2.0, 2.0, 0.0, 1.0 );
		return;
	}

	//An oriented box around the capsule, padded by Extent sigma on all sides.
	float halfAlong  = 0.5 * len + Extent * sigma;
	float halfAcross = Extent * sigma;

	//Corners from the vertex index, as a triangle strip: 0 -> (-,-), 1 -> (+,-),
	//2 -> (-,+), 3 -> (+,+). No vertex buffer for four signs.
	float sx = ( ( gl_VertexID & 1 ) == 0 ) ? -1.0 : 1.0;
	float sy = ( ( gl_VertexID & 2 ) == 0 ) ? -1.0 : 1.0;

	vec2 centre = a + dir * ( 0.5 * len );
	vec2 perp   = vec2( -dir.y, dir.x );
	vec2 pos    = centre + dir * ( sx * halfAlong ) + perp * ( sy * halfAcross );

	segLength = len;
	segSigma  = sigma;
	segEnergy = energy;
	segUV     = vec2( sx * halfAlong, sy * halfAcross );

	//Beam units are isotropic -- one unit is half the face height in both axes --
	//so the spot is round and stays round on a 4:3 face. The divide is the only
	//place the face's shape enters the trace at all.
	gl_Position = vec4( pos.x / FaceAspect, pos.y, 0.0, 1.0 );
}
`;

const TRACE_FRAGMENT_BODY = `
flat in float segLength;
flat in float segSigma;
flat in float segEnergy;
in vec2 segUV;

out vec4 fragColor;

void main()
{
	float inv = 1.0 / segSigma;
	float u   = segUV.x;
	float v   = segUV.y;

	//Across the segment: a normalised Gaussian, minus the value it has at the
	//quad's own boundary.
	//
	//Without the subtraction the profile is cut off at 4.5 sigma with a step of
	//about 1.5e-5 of the peak. For one segment that is invisible. At a turnaround
	//where a thousand segments overlap it is a thousand times 1.5e-5 along a line
	//that is exactly where those thousand boxes end -- a visible polygon edge in
	//the middle of the brightest part of the picture, which is precisely the part
	//the plugin exists to render.
	float across   = InvSqrt2Pi * inv * exp( -0.5 * v * v * inv * inv );
	float pedestal = InvSqrt2Pi * inv * exp( -0.5 * Extent * Extent );
	across = max( across - pedestal, 0.0 );

	float along;
	if( segLength < 0.25 * segSigma )
	{
		//The point limit, taken explicitly rather than left to the CDF.
		//
		//A difference of two nearly equal numbers, each carrying the tanh
		//approximation's ~3e-4 of error, is a difference whose own error is
		//several percent by the time L is a quarter of a sigma. And it is a
		//short segment precisely at the turnarounds and the stationary dots --
		//the features this plugin is about. The limit costs one exp and is exact.
		//
		//\`segLength\` is flat, so this branch is dynamically uniform across the
		//primitive and the derivatives in neighbouring passes stay defined.
		along = InvSqrt2Pi * inv * exp( -0.5 * u * u * inv * inv );
	}
	else
	{
		float halfLen = 0.5 * segLength;//\`half\` is a GLSL reserved word
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	//R is the fast layer's excitation. The slow layer is not deposited into: it
	//is pumped only by what the fast layer sheds, in the decay pass.
	float deposit = segEnergy * across * along;
	fragColor = vec4( deposit, 0.0, 0.0, 0.0 );
}
`;

const DECAY_FRAGMENT_BODY = `
uniform sampler2D HistoryTexture;
uniform float DecayFast;
uniform float DecaySlow;
uniform float Transfer;
uniform float Ceiling;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec2 h = texture( HistoryTexture, uv ).rg;

	//The backstop. Every other guard in the renderer is in front of this buffer;
	//this one is behind it, because a ping-ponged accumulator is the one place
	//where a single bad value is permanent. NaN * DecayFast is NaN, forever, and
	//the operator's only remedy would be to delete the effect and add it again.
	//
	//Comparisons rather than isnan/isinf, for the reason given in Trace.cpp.
	if( !( h.x > -1e30 && h.x < 1e30 ) )
		h.x = 0.0;
	if( !( h.y > -1e30 && h.y < 1e30 ) )
		h.y = 0.0;

	float fast = h.r * DecayFast;
	float slow = h.g * DecaySlow + h.r * ( 1.0 - DecayFast ) * Transfer;

	//The ceiling is not the saturation curve -- that is applied at readout, in
	//the prelude, and it is what actually shapes a bright trace. This is only
	//here so that a runaway cannot climb until it reaches the top of a 32-bit
	//float and becomes an inf that the check above would then have to catch every
	//frame for the rest of the session.
	fragColor = vec4( min( fast, Ceiling ), min( slow, Ceiling ), 0.0, 0.0 );
}
`;

const BRIGHT_FRAGMENT_BODY = `
uniform sampler2D PhosphorTexture;
uniform vec2 SourceSize;
uniform vec2 FaceHalf;      //face half-extents in beam units
uniform float Threshold;

in vec2 uv;

out vec4 fragColor;

void main()
{
	//Four bilinear taps at the corners of the destination footprint: a box
	//downsample that does not leave stair-stepping in the halo.
	vec2 texel = 1.0 / max( SourceSize, vec2( 1.0 ) );
	vec3 sum = phosphorEmission( texture( PhosphorTexture, uv + vec2( -1.0, -1.0 ) * texel ).rg )
	         + phosphorEmission( texture( PhosphorTexture, uv + vec2(  1.0, -1.0 ) * texel ).rg )
	         + phosphorEmission( texture( PhosphorTexture, uv + vec2( -1.0,  1.0 ) * texel ).rg )
	         + phosphorEmission( texture( PhosphorTexture, uv + vec2(  1.0,  1.0 ) * texel ).rg );
	vec3 colour = sum * 0.25;

	colour += GraticuleColour * graticuleAt( ( uv * 2.0 - 1.0 ) * FaceHalf );

	float luma = dot( colour, vec3( 0.299, 0.587, 0.114 ) );
	colour *= smoothstep( Threshold, Threshold + 0.35, luma );

	fragColor = vec4( max( colour, vec3( 0.0 ) ), 1.0 );
}
`;

const BLUR_FRAGMENT_BODY = `
uniform sampler2D SourceTexture;
uniform vec2 Direction;//one texel along the axis being blurred

in vec2 uv;

out vec4 fragColor;

void main()
{
	const float offsets[ 3 ] = float[]( 0.0, 1.3846153846, 3.2307692308 );
	const float weights[ 3 ] = float[]( 0.2270270270, 0.3162162162, 0.0702702703 );

	vec3 sum = texture( SourceTexture, uv ).rgb * weights[ 0 ];
	for( int i = 1; i < 3; ++i )
	{
		sum += texture( SourceTexture, uv + Direction * offsets[ i ] ).rgb * weights[ i ];
		sum += texture( SourceTexture, uv - Direction * offsets[ i ] ).rgb * weights[ i ];
	}

	fragColor = vec4( sum, 1.0 );
}
`;

const GLASS_FRAGMENT_BODY = `
uniform sampler2D PhosphorTexture;
uniform sampler2D BloomTexture;
uniform sampler2D ClipTexture;

uniform vec2  OutputSize;
uniform vec2  FaceHalf;      //face half-extents in beam units: (aspect, 1)
uniform float FaceFit;       //beam units -> square output units
uniform float CornerRadius;  //0..1 of the shorter half-extent; 1 on a square face is a circle
uniform float Curvature;
uniform float Vignette;
uniform float Halation;
uniform float PerspectiveX;
uniform float PerspectiveY;

uniform vec3  FilterTransmission;//what the contrast filter passes, per channel
uniform float FaceBlack;         //how much of the faceplate is actually in front
uniform float Opacity;

uniform float HasClip;  //0 for the source build, which has no input at all
uniform vec2  ClipMaxUV;//the host's padding, applied only where the clip is read

in vec2 uv;

out vec4 fragColor;

const float FOCAL = 2.4;

mat3 rotationX( float a )
{
	float s = sin( a ), c = cos( a );
	return mat3( 1.0, 0.0, 0.0,
	             0.0, c, s,
	             0.0, -s, c );
}

mat3 rotationY( float a )
{
	float s = sin( a ), c = cos( a );
	return mat3( c, 0.0, -s,
	             0.0, 1.0, 0.0,
	             s, 0.0, c );
}

void main()
{
	float aspect = OutputSize.x / max( OutputSize.y, 1.0 );

	//----------------------------------------------------------------------
	// 1. Undo the view.
	//----------------------------------------------------------------------
	vec2 p = uv * 2.0 - 1.0;
	p.x *= aspect;//square units, so a rotation is a rotation

	mat3 orientation = rotationY( PerspectiveX ) * rotationX( PerspectiveY );
	vec3 dir    = vec3( p, FOCAL );
	vec3 normal = orientation * vec3( 0.0, 0.0, 1.0 );
	vec3 centre = vec3( 0.0, 0.0, FOCAL );

	//Guarded rather than branched: an early return here would leave the
	//derivatives below undefined for the whole quad, and both the face edge and
	//the graticule are anti-aliased on them.
	float denom = dot( normal, dir );
	denom = denom >= 0.0 ? max( denom, 1e-4 ) : min( denom, -1e-4 );
	float t = dot( normal, centre ) / denom;

	vec3 local    = transpose( orientation ) * ( t * dir - centre );
	float inFront = step( 1e-4, t );//the face is behind the eye at absurd angles

	//At zero perspective this is exact: the rotations are identity, t is exactly
	//one, and local.xy comes back as p unchanged.
	vec2 viewed = local.xy / max( FaceFit, 1e-6 );

	//----------------------------------------------------------------------
	// 2. Undo the curvature. The face bulges, so the sampling pinches.
	//----------------------------------------------------------------------
	//
	// Divided through by the expansion at the corner, which is overscan: a set
	// deliberately scans a raster larger than its own face so the picture reaches
	// the bezel on all four sides. Without it the distortion pulls the corners
	// inside the glass and shows black beyond them, and the shape you see at the
	// edge becomes an artefact of the curvature rather than the shape of the
	// tube -- which leaves Corner Radius doing nothing at any useful curvature.
	//
	// The coupling to Curvature is real: more distortion needs more overscan to
	// cover the same face. Kept exactly as \`old-cathode\` has it, so the two
	// plugins' tubes are the same tube.
	vec2 n = viewed / FaceHalf;//+-1 at the face edges, so 0.5*dot at the corner is 1
	float bulge = ( 1.0 + Curvature * 0.5 * dot( n, n ) ) / ( 1.0 + Curvature );
	vec2 face = viewed * bulge;

	vec2 faceUV = face / ( 2.0 * FaceHalf ) + 0.5;

	//----------------------------------------------------------------------
	// 3. The glass itself.
	//----------------------------------------------------------------------
	//
	// A rounded rectangle in the tube's own coordinates, so the bezel keeps its
	// shape when the set is turned away from you. Radius 1 on a square face
	// reduces this exactly to length(face) - 1, which is a circle -- which is why
	// a lab scope and a television are the same geometry with different numbers
	// rather than two code paths.
	float radius = clamp( CornerRadius, 0.0, 1.0 ) * min( FaceHalf.x, FaceHalf.y );
	vec2 q   = abs( face ) - ( FaceHalf - radius );
	float sd = length( max( q, vec2( 0.0 ) ) ) + min( max( q.x, q.y ), 0.0 ) - radius;
	float aa = max( fwidth( sd ), 1e-5 );
	float faceMask = ( 1.0 - smoothstep( -aa, aa, sd ) ) * inFront;

	//Curvature can push the sample outside the face buffer before the mask cuts
	//it off, and a clamped texture edge out there would smear the last row of
	//phosphor across the bezel.
	vec2 beyond = step( vec2( 0.0 ), -faceUV ) + step( vec2( 1.0 ), faceUV );
	faceMask *= 1.0 - clamp( beyond.x + beyond.y, 0.0, 1.0 );

	//1.0 - 0.0 * anything is exactly 1.0.
	float vig = 1.0 - Vignette * smoothstep( 0.25, 1.5, length( n * vec2( 0.92, 1.0 ) ) );

	vec3 emission = phosphorEmission( texture( PhosphorTexture, faceUV ).rg ) * vig;
	emission += GraticuleColour * graticuleAt( face );
	emission *= faceMask;

	vec3 halo = texture( BloomTexture, faceUV ).rgb * faceMask;

	//----------------------------------------------------------------------
	// 4. What is behind the glass.
	//----------------------------------------------------------------------
	vec4 clipTexel = vec4( 0.0 );
	if( HasClip > 0.5 )
	{
		//The warp, undone back into output coordinates. At zero curvature and
		//zero perspective the round trip through \`aspect\` is a multiply and a
		//divide that are not required to cancel in floating point, so that case
		//takes the coordinate it already has instead of one that is nearly it.
		vec2 clipUV;
		if( Curvature != 0.0 || PerspectiveX != 0.0 || PerspectiveY != 0.0 )
		{
			vec2 warped = local.xy * bulge;
			clipUV = vec2( warped.x / aspect, warped.y ) * 0.5 + 0.5;
		}
		else
		{
			clipUV = uv;
		}
		clipTexel = texture( ClipTexture, clipUV * ClipMaxUV );
	}

	//The filter carries its tint into the colour and only the mask into the
	//alpha: a green contrast filter darkens what is behind it without making it
	//any less opaque. Both are literal ones when the faceplate is not there.
	vec3 faceplate   = vec3( 1.0 );
	float faceplateA = 1.0;
	if( FaceBlack > 0.0 )
	{
		faceplate  = mix( vec3( 1.0 ), FilterTransmission * faceMask, FaceBlack );
		faceplateA = mix( 1.0, faceMask, FaceBlack );
	}

	vec3 through   = clipTexel.rgb * faceplate;
	float throughA = clipTexel.a * faceplateA;

	vec3 emitted = emission + halo * Halation;
	vec3 colour  = through + emitted;

	//----------------------------------------------------------------------
	// Alpha, premultiplied, which is what Resolume composites in.
	//----------------------------------------------------------------------
	//
	// The tube's own light is opaque -- glass that is emitting is not something
	// you see through. Beyond that:
	//
	//   source build: the face is the object, so alpha is the face mask and the
	//                 layer below shows around it.
	//   effect build: the clip keeps its own alpha, and the faceplate is only as
	//                 opaque as FaceBlack says it is. At FaceBlack 0 that is
	//                 zero, which is what makes the passthrough exact for a
	//                 semi-transparent clip as well as for an opaque one.
	float emissive = clamp( max( emitted.r, max( emitted.g, emitted.b ) ), 0.0, 1.0 );
	float coverage = HasClip > 0.5 ? max( FaceBlack * faceMask, emissive ) : faceMask;

	float alpha = clamp( max( throughA, coverage ), 0.0, 1.0 ) * Opacity;

	//Only the floor is clamped. Nothing here can produce negative light, so a
	//negative value means the clip arrived with one; clamping the ceiling as well
	//would quietly change a passthrough of any clip that carries values above 1,
	//which some do.
	fragColor = vec4( max( colour, vec3( 0.0 ) ), alpha );
}
`;

//---------------------------------------------------------------------------
// Assembly, exactly as Prelude.cpp does it.
//
// Two preludes and not one, for the reason that file gives: `fwidth` appears in
// the graticule, and a vertex stage compiles the whole body whether or not it
// calls the function — so the numeric constants are shared by both stages and
// the helpers only by the fragment stages.
//---------------------------------------------------------------------------

const vertexSource = (body) => CONSTANTS + body;
const fragmentSource = (body) => CONSTANTS + FRAGMENT_HELPERS + body;
const SCREEN_VERTEX = vertexSource(SCREEN_VERTEX_BODY);

//===========================================================================
// The circuit — a port of source/signal/.
//
// JavaScript numbers are IEEE doubles, so the plugin's "double, not float"
// discipline through the whole signal path comes free here. The one thing that
// does not come free is the solved comparator crossing, and that is the part
// this file is most careful about.
//===========================================================================

const clamp = (v, low, high) => (v < low ? low : v > high ? high : v);

function smoothstep01(edge0, edge1, x) {
  const t = clamp((x - edge0) / (edge1 - edge0 || 1e-9), 0, 1);
  return t * t * (3 - 2 * t);
}

/// The largest block the engine will ever synthesise in one frame. Signal.h:
/// the rate is capped at 384 kHz and the frame at 1/24 s, which is 16000
/// samples exactly; the rest is headroom.
const MAX_BLOCK = 16384;

const CHANNELS = 6;

/// The output stage's slew. ~100 ns rise time is a 45 ns time constant, which
/// is under one interval at every rate this engine runs at.
const SLEW_TAU = 45e-9;

/// What pin 7 discharges the capacitor toward. Ground, as the datasheet's
/// 0.693 assumes: a saturated transistor's tenth of a volt would put the
/// trigger crossing 1.2% early at 9 V.
const SINK_VOLTS = 0;

/// How many comparator crossings one interval may contain before the loop gives
/// up. A channel running a thousand times faster than the engine rate can flip
/// that often per interval; the picture is a smear whatever happens, and the
/// bound stops it being a hang.
const MAX_FLIPS_PER_STEP = 64;

function relax(value, target, dt, tau) {
  if (!(tau > 0)) return target;
  return target + (value - target) * Math.exp(-dt / tau);
}

/**
 * One 555 in astable, at the component level — a port of Timer555.cpp.
 *
 * The state is a capacitor voltage and a flip-flop, and the output is whatever
 * the two comparators make of them. That is what lets pin 5, pin 4 and the
 * mark/space pot be inputs to the same circuit rather than three special cases,
 * and it is why the period comes out at 0.693 (Ra + 2Rb) C without that number
 * appearing anywhere below.
 */
class Timer555 {
  constructor() {
    this.p = {
      rCharge: 14700,
      rDischarge: 4700,
      c: 100e-9,
      vcc: 9,
      reset: true, //pin 4
      filterTau: 0,
    };
    this.outHigh = 7.3;
    this.outLow = 0.1;
    this.reset();
  }

  setParams(params) {
    const p = this.p;
    p.rCharge = Math.max(params.rCharge, 1);
    p.rDischarge = Math.max(params.rDischarge, 1);
    p.c = Math.max(params.c, 1e-13);
    p.vcc = Math.max(params.vcc, 1);
    p.reset = params.reset;
    p.filterTau = Math.max(params.filterTau, 0);

    // A bipolar 555's output transistor drops about 1.7 V high and sits about
    // 0.1 V low. A CMOS part would go rail to rail; this is the part on the
    // breadboard in the video.
    this.outHigh = Math.max(p.vcc - 1.7, 0.5);
    this.outLow = 0.1;
  }

  /// Back to the moment of power-up: C empty, output high, nothing in the filter.
  reset() {
    this.vc = 0;
    this.vout = this.outLow;
    this.vfilt = this.outLow;
    this.high = true;
    this.now = 0;
    this.lastRise = -1;
    this.lastFall = -1;
    this.lastPeriod = 0;
    this.lastHighTime = 0;
  }

  /**
   * Advance one interval. `vControl` is the voltage on pin 5, which IS the
   * threshold; the trigger is half of it.
   */
  step(dt, vControl) {
    if (!(dt > 0)) return;
    const p = this.p;

    // The clamp keeps a driven pin 5 inside the range where the part still
    // oscillates: a threshold at or above Vcc is one the capacitor can never
    // reach, and a real 555 driven that hard simply stalls high — which is also
    // what this does, at the top of the clamp, only slowly.
    const vth = clamp(vControl, 0.1 * p.vcc, 0.95 * p.vcc);
    const vtrig = 0.5 * vth;

    let remaining = dt;

    if (!p.reset) {
      // Pin 4 low: the flip-flop is held reset, pin 7 sinks, and the capacitor
      // drains through Rb. The trigger comparator cannot set it again until
      // reset is released.
      this.high = false;
      this.vc = relax(this.vc, SINK_VOLTS, remaining, p.rDischarge * p.c);
      remaining = 0;
    }

    for (let flips = 0; remaining > 0 && flips < MAX_FLIPS_PER_STEP; flips += 1) {
      const target = this.high ? p.vcc : SINK_VOLTS;
      const tau = (this.high ? p.rCharge : p.rDischarge) * p.c;
      const level = this.high ? vth : vtrig;

      // Time to the comparator from here, in closed form:
      // v(t) = target + (vc - target) e^{-t/tau}, so the level is crossed at
      // t = tau ln((vc - target) / (level - target)). The ratio is above one
      // exactly when the level lies between vc and the target, which is the only
      // case in which the crossing exists at all.
      const num = this.vc - target;
      const den = level - target;
      const reaches =
        (this.high ? this.vc < level && level < target : this.vc > level && level > target) &&
        num / den > 1;
      const tCross = reaches ? tau * Math.log(num / den) : -1;

      if (!reaches || tCross >= remaining) {
        this.vc = relax(this.vc, target, remaining, tau);
        this.now += remaining;
        remaining = 0;
        break;
      }

      // Land exactly on the level, at the exact time, and flip.
      this.vc = level;
      this.now += tCross;
      remaining -= tCross;

      if (this.high) {
        //Threshold reached: output falls, discharge begins.
        this.high = false;
        if (this.lastRise >= 0) this.lastHighTime = this.now - this.lastRise;
        this.lastFall = this.now;
      } else {
        //Trigger reached: output rises, charge begins.
        this.high = true;
        if (this.lastRise >= 0) this.lastPeriod = this.now - this.lastRise;
        this.lastRise = this.now;
      }
    }

    if (remaining > 0) {
      // Hit the flip bound. Whatever is left of the interval is spent at the
      // comparator level the loop stopped on; the timing is wrong for this one
      // interval and right again from the next.
      this.now += remaining;
    }

    // The output stage and the RC after it, once per interval, on the state the
    // flip-flop ended the interval in.
    this.vout = relax(this.vout, this.high ? this.outHigh : this.outLow, dt, SLEW_TAU);
    this.vfilt = p.filterTau > 0 ? relax(this.vfilt, this.vout, dt, p.filterTau) : this.vout;
  }
}

/**
 * Six timers, a patch bay and a yoke — a port of Bench.cpp. The breadboard.
 *
 * **The engine rate is chosen from the parts.** A 555 at 1 Hz and one at 50 kHz
 * cannot share a sample rate that suits both, so the rate is picked per frame
 * from the fastest *running* channel: 96 samples of its period, clamped to
 * 24..384 kHz. Changing it between frames costs nothing, because the timers'
 * state is a voltage and not a sample index.
 *
 * **The yoke amplifier is capacitor-coupled**, 2 s, because a 555's output is
 * not symmetric about Vcc/2 and a DC-coupled amplifier would put the figure off
 * centre and move it whenever Vcc changed. Offset is added AFTER the coupling,
 * as the amplifier's centring pot is. One consequence is physical and worth
 * knowing: a figure whose duty changes moves, because its mean has.
 */
const SAMPLES_PER_PERIOD = 96;
const MIN_RATE = 24000;
const MAX_RATE = 384000;
const COUPLING_TAU = 2.0;

class Bench {
  constructor() {
    this.timers = [];
    for (let i = 0; i < CHANNELS; i += 1) this.timers.push(new Timer555());
    this.params = null;
    this.rate = MIN_RATE;
    this.meanX = 0;
    this.meanY = 0;
    this.coilX = 0;
    this.coilY = 0;
    // Four floats a sample: x, y volts, z grid, dt seconds — the two vec4
    // instance attributes the trace vertex stage reads.
    this.block = new Float32Array(MAX_BLOCK * 4);
  }

  reset() {
    for (const t of this.timers) t.reset();
    this.meanX = this.meanY = 0;
    this.coilX = this.coilY = 0;
  }

  /// Cheap; called every frame. Does NOT clear state: turning a knob on a
  /// running bench must not restart the timers.
  setParams(wanted) {
    this.params = wanted;

    let fastest = 0;
    for (let i = 0; i < CHANNELS; i += 1) {
      const ch = wanted.channel[i];
      this.timers[i].setParams(ch);
      // Only a running channel needs resolving. One held in reset is a flat line
      // at whatever rate it is sampled.
      if (ch.reset && ch.nominalPeriod > 0) fastest = Math.max(fastest, 1 / ch.nominalPeriod);
    }

    this.rate = clamp(SAMPLES_PER_PERIOD * fastest, MIN_RATE, MAX_RATE);
  }

  /// What a CV source presents, in -1..1: what pin 5 is pushed by before depth.
  cvSignal(source) {
    if (source <= 0) return 0;
    if (source <= CHANNELS) {
      // An output: -1 low, +1 high in terms of the supply. A bipolar part's high
      // is short of the rail, so this never quite reaches +1 — which is what the
      // next timer's pin 5 actually sees.
      const t = this.timers[source - 1];
      return clamp(
        (t.vout - 0.5 * t.outHigh - 0.5 * t.outLow) / (0.5 * (t.outHigh - t.outLow)),
        -1,
        1,
      );
    }
    if (source <= 2 * CHANNELS) {
      // A capacitor: Vcc/3 .. 2Vcc/3 spans -1..+1, so a stock timer's cap
      // modulates by its whole swing rather than by a third of it.
      const channel = source - 1 - CHANNELS;
      const vcc = this.params.channel[channel].vcc;
      return clamp((this.timers[channel].vc - 0.5 * vcc) / (vcc / 6), -1, 1);
    }
    // Audio: 0..1, pushing the pin UP from rest, so silence is the stock circuit.
    // Permanently zero on this page — see the header.
    return clamp(this.params.audio, 0, 1);
  }

  /// Pin 5's voltage for one channel this interval.
  controlVoltage(channel) {
    const ch = this.params.channel[channel];
    const rest = ch.vcc * (2 / 3);
    if (ch.cvSource <= 0 || !(ch.cvDepth > 0)) return rest;

    // Pin 5 sits at 2/3 Vcc through the internal divider. Driving it through a
    // resistor pulls it toward the source; depth is that resistor. The 0.45 is
    // how far a full-depth source can move it — to 0.37 Vcc or 0.97 Vcc — and the
    // timer's own clamp keeps the top end oscillating.
    return rest * (1 + 0.45 * ch.cvDepth * this.cvSignal(ch.cvSource));
  }

  /// A patch source in deflection volts: (v - Vcc/2) / (Vcc/2), times Level.
  patchSignal(source) {
    if (source <= 0) return 0;

    const kind = Math.floor((source - 1) / CHANNELS); //0 output, 1 cap, 2 filtered
    const channel = (source - 1) % CHANNELS;
    const t = this.timers[channel];
    const ch = this.params.channel[channel];

    const v = kind === 0 ? t.vout : kind === 1 ? t.vc : t.vfilt;

    // The same scale for every kind of signal, so a capacitor (a third of the
    // swing) really is a third the size of an output on the screen, and Gain is
    // what makes it bigger.
    return ((v - 0.5 * ch.vcc) / (0.5 * ch.vcc)) * ch.level;
  }

  /// Synthesise `n` samples covering `frameSeconds` of engine time.
  render(n, frameSeconds) {
    n = clamp(n, 2, MAX_BLOCK) | 0;
    const dt = frameSeconds / (n - 1);

    const patch = this.params.patch;
    const yoke = this.params.yoke;
    const block = this.block;

    const zOf = () => {
      if (patch.zMode === 0 || patch.zSource <= 0) return 1;
      // Z reads the raw signal, not the coupled one: blanking on "pin 3 low" has
      // to mean pin 3, not the yoke.
      const s = this.patchSignal(patch.zSource);
      if (patch.zMode === 1) return s > 0 ? 1 : 0;
      return clamp(0.5 * (s + 1), 0, 1);
    };

    const rail = Math.max(yoke.rail, 1e-3);

    // `axis` is 0 for X and 1 for Y. Written as one function over an index
    // rather than two, exactly as the C++ lambda is called twice: the coupling
    // memory and the coil current are per-axis state that has to survive the
    // block, so they live on `this`.
    const drive = (source, gain, offset, axis, coilTau, stepDt) => {
      let d = this.patchSignal(source) * gain * yoke.deflectionGain;

      // The coupling capacitor: subtract the running mean. Then the centring
      // pot, then the amplifier runs out of headroom, then the coil follows.
      let mean = axis === 0 ? this.meanX : this.meanY;
      if (stepDt > 0) mean = relax(mean, d, stepDt, COUPLING_TAU);
      if (axis === 0) this.meanX = mean;
      else this.meanY = mean;

      d -= mean;
      d += offset;
      d = clamp(d, -rail, rail);

      let coil = axis === 0 ? this.coilX : this.coilY;
      if (stepDt > 0) coil = relax(coil, d, stepDt, coilTau);
      else if (!(coilTau > 0)) coil = d;
      if (axis === 0) this.coilX = coil;
      else this.coilY = coil;

      return coil;
    };

    const v5 = new Array(CHANNELS).fill(0);

    for (let i = 0; i < n; i += 1) {
      //Sample 0 is the state the last block left, un-stepped. See Bench.h.
      const stepDt = i > 0 ? dt : 0;
      if (i > 0) {
        // Every channel's pin 5 is read before any channel steps, so the order
        // of the six on the breadboard does not matter — a timer modulating the
        // one before it sees the same one-interval lag as a timer modulating the
        // one after.
        for (let c = 0; c < CHANNELS; c += 1) v5[c] = this.controlVoltage(c);
        for (let c = 0; c < CHANNELS; c += 1) this.timers[c].step(dt, v5[c]);
      }

      const o = i * 4;
      block[o] = drive(patch.xSource, patch.xGain, patch.xOffset, 0, yoke.coilX, stepDt);
      block[o + 1] = drive(patch.ySource, patch.yGain, patch.yOffset, 1, yoke.coilY, stepDt);
      block[o + 2] = zOf();
      block[o + 3] = dt;
    }

    return block;
  }
}

//===========================================================================
// Phosphor — a port of render/Phosphor.cpp.
//
// A measured table, not a set of tints. Two consequences read as bugs to anyone
// who has not read that file: **P4 at persistence x1 shows no trail at 60 fps**,
// because both of its decay constants are tens of microseconds and a frame is
// sixteen milliseconds — which is the truth of a television, and why the videos
// this plugin is after were shot with the camera's shutter doing the
// persistence; and **changing phosphor changes the brightness by up to three
// times**, because P11's luminous efficiency really is about a third of P31's.
//
// P4 and P22 are astable's own additions at the front of vectrix's six. P22 is
// the white a colour set's three phosphors make together — a single-beam model
// cannot do a shadow mask — with the one property that survives being seen as
// one phosphor: the red is an order of magnitude slower, so a moving spot leaves
// a faint red lag.
//===========================================================================

const PHOSPHORS = [
  { name: 'P4', tauFast: 10.9e-6, fast: [0.82, 0.90, 1.00], tauSlow: 26.0e-6, slow: [1.00, 0.95, 0.55], transfer: 0.60, efficiency: 0.90, saturation: 8.0 },
  { name: 'P22', tauFast: 18.0e-6, fast: [0.62, 1.00, 1.00], tauSlow: 434.0e-6, slow: [1.00, 0.15, 0.10], transfer: 0.50, efficiency: 0.70, saturation: 6.0 },
  { name: 'P31', tauFast: 16.0e-6, fast: [0.18, 1.00, 0.38], tauSlow: 400.0e-6, slow: [0.22, 1.00, 0.42], transfer: 0.02, efficiency: 1.00, saturation: 12.0 },
  { name: 'P1', tauFast: 10.4e-3, fast: [0.22, 1.00, 0.34], tauSlow: 0, slow: [0, 0, 0], transfer: 0, efficiency: 0.55, saturation: 6.0 },
  { name: 'P2', tauFast: 15.0e-3, fast: [0.30, 1.00, 0.40], tauSlow: 52.0e-3, slow: [0.55, 1.00, 0.22], transfer: 0.35, efficiency: 0.62, saturation: 6.0 },
  { name: 'P7', tauFast: 39.0e-6, fast: [0.28, 0.38, 1.00], tauSlow: 174.0e-3, slow: [0.72, 1.00, 0.20], transfer: 0.75, efficiency: 0.48, saturation: 4.0 },
  { name: 'P11', tauFast: 22.0e-6, fast: [0.22, 0.42, 1.00], tauSlow: 0, slow: [0, 0, 0], transfer: 0, efficiency: 0.35, saturation: 8.0 },
  { name: 'P39', tauFast: 65.0e-3, fast: [0.24, 1.00, 0.32], tauSlow: 0, slow: [0, 0, 0], transfer: 0, efficiency: 0.85, saturation: 3.0 },
];

/// Controls.cpp's kPhosphorNames, in table order. The element value is the table
/// index, so this list may only ever grow at the end.
const PHOSPHOR_NAMES = [
  'P4 White', 'P22 Colour', 'P31 Green', 'P1 Green', 'P2 Long Green', 'P7 Blue/Amber', 'P11 Blue', 'P39 Very Long',
];

/// A decay factor of exactly 1 is a buffer that never empties, and the operator
/// has no way back from it short of deleting the effect.
const MAX_DECAY = 0.9995;

function decayFactor(tau, frameSeconds) {
  if (!(tau > 0) || !(frameSeconds > 0)) return 0;
  // No guard on the exponent's magnitude: exp() of a large negative number
  // underflows to zero, which is the correct answer — a phosphor whose tau is a
  // thousandth of a frame really has gone out.
  return clamp(Math.exp(-frameSeconds / tau), 0, MAX_DECAY);
}

function decayFor(spec, persistence, frameSeconds) {
  const mult = Math.max(persistence, 1e-4);
  const fast = decayFactor(spec.tauFast * mult, frameSeconds);

  // A phosphor with no second layer gets a zero decay and a zero transfer, not a
  // tiny one. Otherwise the slow channel accumulates a hundredth of the trace
  // every frame and never quite lets go of it.
  if (spec.tauSlow > 0 && spec.transfer > 0) {
    return { fast, slow: decayFactor(spec.tauSlow * mult, frameSeconds), transfer: clamp(spec.transfer, 0, 1) };
  }
  return { fast, slow: 0, transfer: 0 };
}

/// x0.1 to x1000: four decades, with x1 a quarter of the way up. Interpolated in
/// the log, so the control's feel is the same everywhere along it.
const persistenceMultiplier = (normalised) => Math.pow(10, -1 + 4 * clamp(normalised, 0, 1));

/// Where x1 sits on that control, which is where its default belongs.
const PERSISTENCE_UNITY = 0.25;

//===========================================================================
// The face — a port of render/Tube.cpp.
//===========================================================================

/// The rungs the face buffer is allowed to sit on. Reallocating it throws away
/// the phosphor history — the trail vanishes and the picture flashes — so an
/// operator dragging Focus would otherwise cross a boundary on almost every
/// mouse move. On the ladder they cross four in the whole travel.
const FACE_LADDER = [512, 768, 1024, 1440, 2160];

/// Texels per sigma below which the spot is visibly a polygon.
const TEXELS_PER_SIGMA = 3;

/// The same rounded-rectangle signed distance the glass shader evaluates, on the
/// CPU, so the graticule is solved against the shape that is actually drawn.
function faceDistance(px, py, halfX, halfY, radius) {
  const qx = Math.abs(px) - (halfX - radius);
  const qy = Math.abs(py) - (halfY - radius);
  const mx = Math.max(qx, 0);
  const my = Math.max(qy, 0);
  return Math.sqrt(mx * mx + my * my) + Math.min(Math.max(qx, qy), 0) - radius;
}

function faceSizeFor(spotFraction, outputHeight) {
  const fraction = Math.max(spotFraction, 1e-4);
  const wanted = Math.ceil(TEXELS_PER_SIGMA / fraction);

  // Up to the first rung that is big enough; the top rung if nothing is.
  let chosen = FACE_LADDER[FACE_LADDER.length - 1];
  for (const rung of FACE_LADDER) {
    if (rung >= wanted) {
      chosen = rung;
      break;
    }
  }

  // And down to the largest rung the output can actually show. Capping to the
  // output height itself would take the result off the ladder, and every Focus
  // change near that boundary would then flash.
  let cap = FACE_LADDER[0];
  for (const rung of FACE_LADDER) {
    if (rung <= outputHeight) cap = rung;
  }

  return Math.min(chosen, cap);
}

const faceWidthFor = (faceHeight, faceAspect) =>
  Math.max(1, Math.round(faceHeight * clamp(faceAspect, 0.05, 20)));

/// Beam units to square output units: whichever constraint binds — width on a
/// wide face in a narrow frame, height otherwise.
const faceFitScale = (outputAspect, faceAspect) =>
  Math.min(Math.max(outputAspect, 1e-4) / Math.max(faceAspect, 1e-4), 1);

/**
 * The size of one graticule division in beam units, for a 10 by 8 graticule
 * inscribed in this face. Bisected rather than solved: the distance field is
 * piecewise, so a closed form needs three cases and a test for which one the
 * corner lands in.
 *
 * Astable's tube has no graticule — a television does not have one, and
 * `GRATICULE` below is zero — but the uniform still has to carry a sane
 * division, because `graticuleAt()` divides by it.
 */
function graticuleDivision(tube) {
  const halfX = Math.max(tube.faceAspect, 1e-4);
  const halfY = 1;
  const radius = clamp(tube.cornerRadius, 0, 1) * Math.min(halfX, halfY);

  let lo = 0;
  let hi = 2 * Math.max(halfX, halfY);
  for (let i = 0; i < 30; i += 1) {
    const mid = 0.5 * (lo + hi);
    if (faceDistance(5 * mid, 4 * mid, halfX, halfY, radius) < 0) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

//===========================================================================
// Controls — a port of source/Controls.cpp.
//
// Every continuous host parameter is 0..1 and mapped here, and that is not a
// stylistic choice: `SetParamInfo` clamps a STANDARD default into 0..1 *before*
// `SetParamRange` can widen it, so a parameter declared in ohms cannot declare a
// default in ohms — 10000 silently becomes 1.
//===========================================================================

/// An option parameter holds its element *value*, not a 0..1 fraction, so it is
/// rounded rather than scaled. Getting this backwards gives a dropdown
/// permanently stuck on its first entry.
const option = (value, count) => clamp(Math.round(value), 0, count - 1);

const expo = (value, low, high) => low * Math.pow(high / low, clamp(value, 0, 1));
const linear = (value, low, high) => low + (high - low) * clamp(value, 0, 1);
const boolean = (v) => v > 0.5;

/// A control whose bottom end is a true zero and whose body is exponential: the
/// exponential alone bottoms out at `low`, never at nothing, so the first 2% of
/// the travel fades it linearly to zero.
const OFF_RAMP = 0.02;

function expoWithOff(value, low, high) {
  const t = clamp(value, 0, 1);
  if (t <= 0) return 0;
  return expo((t - OFF_RAMP) / (1 - OFF_RAMP), low, high) * Math.min(1, t / OFF_RAMP);
}

const DECADE_NAMES = ['1 nF', '10 nF', '100 nF', '1 uF', '10 uF', '100 uF'];
const DECADE_FARADS = [1e-9, 10e-9, 100e-9, 1e-6, 10e-6, 100e-6];

const CV_SOURCE_NAMES = [
  'None',
  'Ch1 Output', 'Ch2 Output', 'Ch3 Output', 'Ch4 Output', 'Ch5 Output', 'Ch6 Output',
  'Ch1 Cap', 'Ch2 Cap', 'Ch3 Cap', 'Ch4 Cap', 'Ch5 Cap', 'Ch6 Cap',
  'Audio',
];

const PATCH_SOURCE_NAMES = [
  'Off',
  'Ch1 Output', 'Ch2 Output', 'Ch3 Output', 'Ch4 Output', 'Ch5 Output', 'Ch6 Output',
  'Ch1 Cap', 'Ch2 Cap', 'Ch3 Cap', 'Ch4 Cap', 'Ch5 Cap', 'Ch6 Cap',
  'Ch1 Filtered', 'Ch2 Filtered', 'Ch3 Filtered', 'Ch4 Filtered', 'Ch5 Filtered', 'Ch6 Filtered',
];

const Z_MODE_NAMES = ['Off', 'Blank When Low', 'Brightness'];

const resistanceOf = (value) => expo(value, 1.0e3, 1.0e6);

/// A decade and a multiplier, so a value can be read off like a real part:
/// "100 nF, times 1.03" is a 103 nF capacitor, which is a 100 nF one at 3%.
const capacitanceOf = (decade, fine) =>
  DECADE_FARADS[option(decade, DECADE_NAMES.length)] * expo(fine, 1, 10);

/// The stock period a channel would have with the pot centred: ln2 (Ra + 2Rb) C.
const nominalPeriod = (ra, rb, c) => 0.6931471805599453 * (ra + 2 * rb) * c;

/**
 * The duty the Mark-Space pot asks for, with the stock duty at its centre.
 *
 * Piecewise linear from 5% at one end, through the stock circuit's own duty at
 * 0.5, to 95% at the other. Piecewise because the centre has to be the stock
 * circuit exactly: `attest --period` asserts the stock formula and it does it at
 * 0.5.
 */
function dutyFor(markSpace, stockDuty) {
  const m = clamp(markSpace, 0, 1);
  if (m < 0.5) return 0.05 + (stockDuty - 0.05) * (m / 0.5);
  return stockDuty + (0.95 - stockDuty) * ((m - 0.5) / 0.5);
}

//---------------------------------------------------------------------------
// The renderer's fixed settings: what vectrix exposes as controls and a
// portable television does not have knobs for. Astable.cpp's own constants, and
// its renderParams(): a television has no graticule and no contrast filter, and
// its face is the object, so the faceplate is fully in front and the layer below
// shows around it.
//---------------------------------------------------------------------------
const SPOT_DEFOCUS = 0.35;
const HALATION = 1.4; //vectrix's 0.35 on its 0..4
const HALATION_RADIUS = 0.5;
const HALATION_THRESHOLD = 0.04;
const CURVATURE = 0.35;
const VIGNETTE = 0.25;
const GRATICULE = 0.0;
const FACE_BLACK = 1.0;
const OPACITY = 1.0;
const BLANK_FLOOR = 0.0;
const DENSITY_FLOOR = 1.0e-4;

//===========================================================================
// Presets — a port of source/Presets.h.
//
// **A preset is an OVERRIDE, not a write.** Resolume does not consume value
// events, so a plugin cannot push a preset's values back into the inspector; if
// it changed its own parameters the sliders would keep showing the old numbers.
// So while the dropdown is on anything but Custom, the row's values are laid
// over the operator's every frame at read time, and the inspector is — for those
// columns — not the truth. That is reproduced here exactly, including the part
// that reads as a bug: drag a slider a preset covers and nothing moves.
//
// A row covers the whole bench — the six channels, the patch bay, the yoke — and
// Brightness. Brightness is in because the energy a frame deposits is fixed, so
// a figure that parks the beam at four corners and one that sweeps it along a
// continuous curve differ in peak brightness by two orders of magnitude. The
// rest of the Tube group stays out: which television this is on is a different
// question from which breadboard you are watching.
//
// **Row 1 is also the defaults**, by construction — `attest --defaults` holds
// the two together in the repository, and `DEFAULTS` below is built from row 1
// here for the same reason.
//===========================================================================

/// Which demo parameter each preset column drives, in `PresetColumn` order:
/// ten per channel, then the patch bay's eight, the yoke's four, and Brightness.
const CHANNEL_CONTROLS = ['ra', 'rb', 'decade', 'fine', 'cv', 'depth', 'ms', 'reset', 'filter', 'level'];

const PRESET_COLUMN_IDS = [];
for (let c = 1; c <= CHANNELS; c += 1) {
  for (const control of CHANNEL_CONTROLS) PRESET_COLUMN_IDS.push(`ch${c}${control}`);
}
PRESET_COLUMN_IDS.push('xSource', 'ySource', 'zSource', 'xGain', 'yGain', 'xOffset', 'yOffset', 'zMode');
PRESET_COLUMN_IDS.push('coilX', 'coilY', 'deflectionGain', 'ampRail');
PRESET_COLUMN_IDS.push('brightness');

const PRESET_COLUMN = new Map(PRESET_COLUMN_IDS.map((id, index) => [id, index]));

//The resistors, as slider positions: log over 1 kR .. 1 MR, so p = log10(R/1k)/3.
const R1K = 0.0, R2K2 = 0.11414, R4K7 = 0.22403, R10K = 0.33333;
const R22K = 0.44715, R47K = 0.55800, R100K = 0.66667;

//Capacitor decades, as element values.
const C1NF = 0, C10NF = 1, C100NF = 2, C1UF = 3, C10UF = 4, C100UF = 5;

//CV sources: 0 none, 1..6 outputs, 7..12 caps, 13 audio.
const CV_NONE = 0, CV_CH4_CAP = 10, CV_CH6_CAP = 12;

//Patch sources: 0 off, 1..6 outputs, 7..12 caps, 13..18 filtered.
const P_OFF = 0, P_OUT1 = 1, P_OUT2 = 2, P_CAP1 = 7, P_CAP2 = 8, P_FILT1 = 13;

/// One channel's ten columns, stock: pot centred, reset on, no filter, full level.
const stock = (ra, rb, decade, fine) => [ra, rb, decade, fine, CV_NONE, 0, 0.5, 1, 0, 1];

const withFilter = (ch, filter) => { const c = ch.slice(); c[8] = filter; return c; };
const withCv = (ch, source, depth) => { const c = ch.slice(); c[4] = source; c[5] = depth; return c; };
const withMarkSpace = (ch, m) => { const c = ch.slice(); c[6] = m; return c; };

/// The six spare-and-default channels every row starts from. 1 and 2 are the two
/// squares at 744 and 737 Hz — a 1% capacitor mismatch, which is what two parts
/// out of the same bag do; 3 is a 2.2 kHz spare; 4 a 7.4 Hz LFO; 5 a 1.6 kHz
/// spare; 6 a 2.2 Hz LFO. The LFOs exist to be CV sources.
const CH1 = stock(R10K, R4K7, C100NF, 0);
const CH2 = stock(R10K, R4K7, C100NF, 0.00432); //x1.01
const CH3 = stock(R22K, R22K, C10NF, 0);
const CH4 = stock(R100K, R47K, C1UF, 0);
const CH5 = stock(R4K7, R2K2, C100NF, 0);
const CH6 = stock(R47K, R10K, C10UF, 0);

const PATCH_DOTS = [P_OUT1, P_OUT2, P_OFF, 0.5, 0.5, 0.5, 0.5, 0];
const YOKE_SCOPE = [0, 0, 0.5, 0.66];

const makePreset = (name, c1, c2, c3, c4, c5, c6, patch, yoke, brightness) => ({
  name,
  v: [...c1, ...c2, ...c3, ...c4, ...c5, ...c6, ...patch, ...yoke, brightness],
});

const PRESETS = [
  // The defaults. Two squares into X and Y: the beam dwells at the rails and
  // crosses between them fast, so the picture is four dots and the faint lines of
  // the crossings. Electrostatic (Coil 0), so the crossings are straight.
  makePreset('Four Dots', CH1, CH2, CH3, CH4, CH5, CH6, PATCH_DOTS, YOKE_SCOPE, 0.50),

  // An RC on channel 1's output, and X taken from after it. The dots become
  // horizontal lines because X now takes its time getting across, and the lines
  // are brightest at their ends because that is where it is slowest.
  makePreset('Lines', withFilter(CH1, 0.45), CH2, CH3, CH4, CH5, CH6,
    [P_FILT1, P_OUT2, P_OFF, 0.55, 0.5, 0.5, 0.5, 0], YOKE_SCOPE, 0.60),

  // The capacitors instead of the outputs: exponentials between Vcc/3 and
  // 2Vcc/3 on both axes, so every edge is a curve rather than a straight run
  // between two rails. Channel 2 gets twice channel 1's period so the figure
  // closes as a two-lobed curve; the gain is up because a capacitor swings a
  // third of what an output does, and the brightness with it.
  makePreset('Curves', CH1, stock(R10K, R4K7, C100NF, 0.30320), CH3, CH4, CH5, CH6,
    [P_CAP1, P_CAP2, P_OFF, 0.85, 0.85, 0.5, 0.5, 0], YOKE_SCOPE, 0.86),

  // Channel 2 at three halves of channel 1 — 66.8 nF against 100 nF — so the
  // figure is a 3:2 Lissajous, and a few hertz off exact so it crawls. A little
  // coil on both axes rounds the exponentials' corners.
  makePreset('Lissajous Crawl', CH1, stock(R10K, R4K7, C10NF, 0.8250), CH3, CH4, CH5, CH6,
    [P_CAP1, P_CAP2, P_OFF, 0.85, 0.85, 0.5, 0.5, 0], [0.3, 0.3, 0.5, 0.66], 0.78),

  // Channel 4's capacitor — the 7 Hz LFO — into channel 1's pin 5. The threshold
  // moves, so channel 1's frequency and duty sweep with it.
  makePreset('FM', withCv(withFilter(CH1, 0.35), CV_CH4_CAP, 0.7), CH2, CH3, CH4, CH5, CH6,
    [P_FILT1, P_CAP2, P_OFF, 0.55, 0.85, 0.5, 0.5, 0], [0.2, 0.2, 0.5, 0.66], 0.74),

  // The mark/space pot pulled off centre on channel 1 and the 2 Hz LFO into its
  // pin 5. The dots' dwell — and, through the coupling capacitor, their position
  // — follows the duty.
  makePreset('Duty Sweep', withCv(withMarkSpace(CH1, 0.75), CV_CH6_CAP, 0.8), CH2, CH3, CH4, CH5, CH6,
    [P_OUT1, P_CAP2, P_OFF, 0.5, 0.85, 0.5, 0.5, 0], [0.25, 0.25, 0.5, 0.66], 0.62),

  // Two ramps: the pot at 95% makes each capacitor a sawtooth. Channel 1 at 744
  // Hz is the line, channel 2 at 25 Hz is the frame, and Z takes channel 1's
  // output with Blank When Low, so the beam is cut during the line retrace. The
  // frame retrace is not blanked; it is one fast diagonal, dim because it is fast.
  makePreset('Raster', withMarkSpace(CH1, 0.95), withMarkSpace(stock(R100K, R47K, C100NF, 0.4728), 0.95),
    CH3, CH4, CH5, CH6,
    [P_CAP1, P_CAP2, P_OUT1, 0.85, 0.85, 0.5, 0.5, 1], [0.15, 0.15, 0.5, 0.66], 0.80),
];

const PRESET_NAMES = ['Custom', ...PRESETS.map((p) => p.name)];

/// The value of column `id` in row 1, which is what the plugin's constructor
/// seeds `params[]` with. Anything outside the preset run has its default given
/// where it is declared, as `kDefaults` does.
const rowOne = (id) => PRESETS[0].v[PRESET_COLUMN.get(id)];

/**
 * `AstablePlugin::Effective`. The one place a preset is read.
 *
 * While the dropdown is on anything but Custom, a covered parameter answers with
 * the row's value rather than the slider's. Everything else answers with the
 * slider.
 */
function effective(params, id) {
  const preset = option(params.get('preset'), PRESET_NAMES.length);
  const column = PRESET_COLUMN.get(id);
  if (preset > 0 && column !== undefined) return PRESETS[preset - 1].v[column];
  return params.get(id);
}

/// `Controls.cpp`'s `Resolve`, over the effective values.
function resolve(params) {
  const p = (id) => effective(params, id);

  const vcc = linear(p('vcc'), 5, 15);

  const bench = { channel: [], patch: {}, yoke: {}, audio: 0 };

  for (let i = 1; i <= CHANNELS; i += 1) {
    const at = (control) => p(`ch${i}${control}`);

    const ra = resistanceOf(at('ra'));
    const rb = resistanceOf(at('rb'));
    const c = capacitanceOf(at('decade'), at('fine'));

    //------------------------------------------------------------------
    // The Maddi mark/space trick.
    //
    // The stock circuit charges through Ra + Rb and discharges through Rb, so
    // its duty is (Ra + Rb) / (Ra + 2Rb) and cannot go below a half. The trick
    // puts a pot across the timing path with a diode steering the charge through
    // one side of the wiper and the discharge through the other, so the two
    // halves of the cycle share one total resistance and the wiper only decides
    // how it is split: the period stays put and the duty goes wherever the wiper
    // is. Modelled as exactly that — the total is the stock Ra + 2Rb, so at the
    // centre the two resistances ARE Ra + Rb and Rb and the stock formula holds
    // to the digit.
    //------------------------------------------------------------------
    const total = ra + 2 * rb;
    const stockDuty = (ra + rb) / total;
    const duty = dutyFor(at('ms'), stockDuty);

    const period = nominalPeriod(ra, rb, c);

    bench.channel.push({
      rCharge: Math.max(total * duty, 1),
      rDischarge: Math.max(total * (1 - duty), 1),
      c,
      vcc,
      nominalPeriod: period,
      cvSource: option(at('cv'), CV_SOURCE_NAMES.length),
      cvDepth: clamp(at('depth'), 0, 1),
      reset: boolean(at('reset')),
      // The filter is measured in the channel's own periods — 0.02 to 4 of them
      // — rather than in seconds, so it means the same thing on a 2 Hz timer as
      // on a 2 kHz one. In seconds it would be a control whose useful range moved
      // five decades depending on which channel it was on.
      filterTau: expoWithOff(at('filter'), 0.02, 4) * period,
      level: clamp(at('level'), 0, 1),
    });
  }

  bench.patch = {
    xSource: option(p('xSource'), PATCH_SOURCE_NAMES.length),
    ySource: option(p('ySource'), PATCH_SOURCE_NAMES.length),
    zSource: option(p('zSource'), PATCH_SOURCE_NAMES.length),
    xGain: expoWithOff(p('xGain'), 0.1, 4),
    yGain: expoWithOff(p('yGain'), 0.1, 4),
    xOffset: linear(p('xOffset'), -1, 1),
    yOffset: linear(p('yOffset'), -1, 1),
    zMode: option(p('zMode'), Z_MODE_NAMES.length),
  };

  bench.yoke = {
    coilX: expoWithOff(p('coilX'), 10e-6, 100e-3),
    coilY: expoWithOff(p('coilY'), 10e-6, 100e-3),
    deflectionGain: linear(p('deflectionGain'), 0, 2),
    rail: expo(p('ampRail'), 0.2, 3),
  };

  // The host's spectrum, folded and smoothed, times Audio Gain. Both are absent
  // from this page, so it is a constant zero and `Audio` as a CV source is a
  // permanent silence. See the header.
  bench.audio = 0;

  const brightness = clamp(p('brightness'), 0, 1);

  const tube = {
    phosphor: option(p('phosphor'), PHOSPHORS.length),
    persistence: persistenceMultiplier(p('persistence')),
    spotSigma: expo(p('focus'), 0.0012, 0.02),
    faceAspect: expo(p('faceAspect'), 1, 2),
    cornerRadius: clamp(p('cornerRadius'), 0, 1),
    // Exponential around a calibrated 1.0, with a true off at the bottom of the
    // travel: vectrix's Beam control, for vectrix's reasons.
    beamPower: Math.pow(10, -1 + 2 * brightness) * Math.min(1, brightness * 50),
    deflectionGain: linear(p('overscan'), 0.6, 1.4),
  };

  return { bench, tube };
}

//===========================================================================
// The renderer: the same six passes, in the same order, as
// BeamGeometry::Render.
//
//   1. Decay      the phosphor buffer into the other one, two layers with a
//                 cascade between them.
//   2. Trace      one instanced quad per interval, additive, into the target the
//                 decay just wrote — so there is no third buffer and no combine.
//   3. Bright     emission plus graticule, thresholded, at quarter size.
//   4/5. Blur     separable Gaussian, ping-ponged, one to three times.
//   6. Glass      the faceplate, onto the canvas. There is no clip behind it:
//                 this is the source build and `HasClip` is 0.
//===========================================================================

/// A runaway cannot be allowed to climb until it reaches the top of a 32-bit
/// float and turns into an inf. Well below that, and far above anything the
/// saturation curve will let through, so it never shapes a picture.
const EXCITATION_CEILING = 1.0e6;

/// A television has no contrast filter. `FaceBlack` is 1, so the shader still
/// multiplies by this — which is why it is a literal one rather than the scope's
/// green glass.
const FILTER_TRANSMISSION = [1, 1, 1];
const GRATICULE_COLOUR = [0.30, 0.42, 0.38];

function createRenderer(gl, quad) {
  const traceShader = new Program(
    gl,
    vertexSource(TRACE_VERTEX_BODY),
    fragmentSource(TRACE_FRAGMENT_BODY),
    'trace',
    // The trace sources its own geometry: two vec4 instance attributes, not the
    // screen quad's position and uv.
    { attribs: { sampleA: 0, sampleB: 1 } },
  );
  const decayShader = new Program(gl, SCREEN_VERTEX, fragmentSource(DECAY_FRAGMENT_BODY), 'decay');
  const brightShader = new Program(gl, SCREEN_VERTEX, fragmentSource(BRIGHT_FRAGMENT_BODY), 'halation bright pass');
  const blurShader = new Program(gl, SCREEN_VERTEX, fragmentSource(BLUR_FRAGMENT_BODY), 'halation blur');
  const glassShader = new Program(gl, SCREEN_VERTEX, fragmentSource(GLASS_FRAGMENT_BODY), 'glass');

  const phosphorBuffer = [new PassBuffer(gl), new PassBuffer(gl)];
  const bloomBuffer = [new PassBuffer(gl), new PassBuffer(gl)];

  //-----------------------------------------------------------------------
  // One buffer of samples, read twice.
  //
  // Attribute 0 starts at the beginning and attribute 1 one element in, so
  // instance i sees sample i and sample i+1 with nothing duplicated and half the
  // bandwidth of an expanded segment list. It costs one thing: the draw must ask
  // for n-1 instances, because n instances would read one sample past the end.
  //
  // vertexAttribDivisor is VAO state, not global state, so it has to be set with
  // this VAO bound. Set it with the wrong one bound and the attributes silently
  // become per-vertex, which looks like corrupt geometry.
  //-----------------------------------------------------------------------
  const traceVAO = gl.createVertexArray();
  const traceVBO = gl.createBuffer();

  gl.bindVertexArray(traceVAO);
  gl.bindBuffer(gl.ARRAY_BUFFER, traceVBO);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 16, 0);
  gl.vertexAttribDivisor(0, 1);
  gl.enableVertexAttribArray(1);
  gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 16, 16);
  gl.vertexAttribDivisor(1, 1);
  gl.bindVertexArray(null);
  gl.bindBuffer(gl.ARRAY_BUFFER, null);

  const bench = new Bench();

  let phosphorIndex = 0;
  let lastTime = null;

  /// Uniforms declared by the shared fragment prelude, and therefore needed by
  /// every pass that turns excitation into light.
  function setPreludeUniforms(shader, phosphorIndexValue, graticuleLevel, graticuleDiv) {
    const spec = PHOSPHORS[clamp(phosphorIndexValue, 0, PHOSPHORS.length - 1)];
    shader.set('FastColour', spec.fast);
    shader.set('SlowColour', spec.slow);
    shader.set('PhosphorEfficiency', spec.efficiency);
    shader.set('PhosphorSaturation', Math.max(spec.saturation, 1e-4));
    shader.set('GraticuleLevel', graticuleLevel);
    shader.set('GraticuleDiv', graticuleDiv);
    shader.set('GraticuleColour', GRATICULE_COLOUR);
  }

  return {
    render({ params, width, height, time }) {
      //------------------------------------------------------------------
      // The clock. Clock.cpp's job, and its clamp is not optional: an unclamped
      // delta after a stalled tab asks the engine for half a second of bench in
      // one block, which is a thirty times energy deposit and a white flash.
      //
      // The kit's Restart button puts `time` back to zero, and time running
      // backwards is the one thing a deflection amplifier cannot do. So it is
      // taken as a power cycle — Clock::Reset and Bench::Reset together — rather
      // than as a very long frame.
      //------------------------------------------------------------------
      const restarted = lastTime !== null && time < lastTime;
      const raw = lastTime === null ? 1 / 60 : time - lastTime;
      lastTime = time;
      const frameSeconds = clamp(raw, 1 / 240, 1 / 24);

      let clearHistory = false;
      if (restarted) {
        bench.reset();
        clearHistory = true;
      }

      const resolved = resolve(params);
      const tube = resolved.tube;

      bench.setParams(resolved.bench);

      // Clock::SamplesForThisFrame. At least two: one sample is a point with no
      // interval after it, and the renderer draws segments.
      const n = clamp(Math.round(frameSeconds * bench.rate), 2, MAX_BLOCK);
      const samples = bench.render(n, frameSeconds);
      const segmentCount = Math.max(0, n - 1);

      //------------------------------------------------------------------
      // Buffers. Sized by the spot, not by the composition — see Tube.h.
      //------------------------------------------------------------------
      const spotSigma = Math.max(tube.spotSigma, 1e-5);
      // Beam units run to 1 at half the face height, so a sigma expressed in them
      // is twice its fraction of the full height. The *undefocused* sigma sets
      // the resolution, because it is the smallest.
      const spotFraction = spotSigma * 0.5;

      const faceAspect = Math.max(tube.faceAspect, 0.05);
      const faceH = faceSizeFor(spotFraction, height);
      const faceW = faceWidthFor(faceH, faceAspect);
      const bloomW = Math.max(1, Math.floor(faceW / 4));
      const bloomH = Math.max(1, Math.floor(faceH / 4));

      // RG32F and not RGBA16F. The two channels are excitations that accumulate
      // over hundreds of frames on a long phosphor, and half-float runs out of
      // mantissa exactly where a bright dwell has been building for a while — the
      // accumulator stops climbing and the brightest part of the picture is the
      // part that stops responding.
      const reallocated = phosphorBuffer[0].width !== faceW || phosphorBuffer[0].height !== faceH;
      phosphorBuffer[0].ensure(faceW, faceH, gl.RG32F);
      phosphorBuffer[1].ensure(faceW, faceH, gl.RG32F);
      bloomBuffer[0].ensure(bloomW, bloomH, gl.RGBA16F);
      bloomBuffer[1].ensure(bloomW, bloomH, gl.RGBA16F);

      // A fresh allocation has no history in it, and the plugin's ScopeBuffer
      // clears on the way in for the same reason.
      if (reallocated) clearHistory = true;

      if (clearHistory) {
        phosphorBuffer[0].clearTo(0, 0, 0, 0);
        phosphorBuffer[1].clearTo(0, 0, 0, 0);
      }

      const target = phosphorIndex;
      const history = 1 - phosphorIndex;

      //------------------------------------------------------------------
      // Everything the shape of the face implies, worked out once.
      //------------------------------------------------------------------
      const outputAspect = width / height;
      const faceFit = faceFitScale(outputAspect, faceAspect);
      const division = graticuleDivision(tube);

      // One square output unit is half the output height in pixels, so this is
      // how many output pixels a division is worth. Below about three of them a
      // graticule is a moire generator, so it fades. Astable's television has no
      // graticule at all — GRATICULE is zero — so this is always zero; it is
      // computed the plugin's way anyway rather than short-circuited, because the
      // shaders are the plugin's and a uniform they read should arrive the way
      // the plugin sends it.
      const pixelsPerDivision = division * faceFit * height * 0.5;
      const graticuleLevel = Math.max(GRATICULE, 0) * smoothstep01(2, 4, pixelsPerDivision);

      const decay = decayFor(
        PHOSPHORS[clamp(tube.phosphor, 0, PHOSPHORS.length - 1)],
        tube.persistence,
        Math.max(frameSeconds, 1.0e-5),
      );

      //------------------------------------------------------------------
      // Upload the block. STREAM_DRAW and a fresh bufferData every frame: the
      // whole point is that the driver orphans the old storage rather than
      // waiting for the previous frame's draw to finish reading it.
      //------------------------------------------------------------------
      gl.bindBuffer(gl.ARRAY_BUFFER, traceVBO);
      gl.bufferData(gl.ARRAY_BUFFER, samples.subarray(0, n * 4), gl.STREAM_DRAW);
      gl.bindBuffer(gl.ARRAY_BUFFER, null);

      //------------------------------------------------------------------
      // 1 and 2. Decay, then deposit, into the same target.
      //------------------------------------------------------------------
      phosphorBuffer[target].bind();

      gl.disable(gl.BLEND);
      decayShader.use();
      bindTexture(gl, 0, phosphorBuffer[history].texture);
      decayShader.setSampler('HistoryTexture', 0);
      decayShader.set('DecayFast', decay.fast);
      decayShader.set('DecaySlow', decay.slow);
      decayShader.set('Transfer', decay.transfer);
      decayShader.set('Ceiling', EXCITATION_CEILING);
      quad.draw();

      if (segmentCount > 0 && tube.beamPower > 0) {
        // Sum, not max(). This input is a *deposit*: the energy one interval of
        // beam put on the glass. Two intervals crossing the same texel really did
        // put twice the energy there, and a max() would silently throw away every
        // crossing in the figure — which on four dots made of two square waves is
        // the whole of the faint structure between them.
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE);

        traceShader.use();
        traceShader.set('BeamPower', tube.beamPower);
        traceShader.set('SpotSigma', spotSigma);
        traceShader.set('SpotDefocus', SPOT_DEFOCUS);
        traceShader.set('BlankFloor', BLANK_FLOOR);
        traceShader.set('DensityFloor', DENSITY_FLOOR);
        traceShader.set('DeflectionGain', tube.deflectionGain);
        traceShader.set('FaceAspect', faceAspect);

        gl.bindVertexArray(traceVAO);
        gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, segmentCount);
        gl.bindVertexArray(null);

        gl.disable(gl.BLEND);
      }

      //------------------------------------------------------------------
      // 3 to 5. Halation, all at quarter size.
      //------------------------------------------------------------------
      bloomBuffer[0].bind();
      gl.disable(gl.BLEND);
      brightShader.use();
      bindTexture(gl, 0, phosphorBuffer[target].texture);
      setPreludeUniforms(brightShader, tube.phosphor, graticuleLevel, division);
      brightShader.setSampler('PhosphorTexture', 0);
      brightShader.set('SourceSize', faceW, faceH);
      brightShader.set('FaceHalf', faceAspect, 1);
      brightShader.set('Threshold', HALATION_THRESHOLD);
      quad.draw();

      // One iteration is a tight halo, three is a wide soft one, and three is
      // also the ceiling: past three the halo is wider than the face and stops
      // being scattering in glass at all.
      const iterations = clamp(1 + Math.round(2 * clamp(HALATION_RADIUS, 0, 1)), 1, 3);

      for (let i = 0; i < iterations; i += 1) {
        // Across then down, so every iteration ends back in bloomBuffer[0] and
        // the glass pass never has to ask which one it landed in.
        const blurs = [
          { from: 0, to: 1, dx: 1 / bloomW, dy: 0 },
          { from: 1, to: 0, dx: 0, dy: 1 / bloomH },
        ];
        for (const pass of blurs) {
          bloomBuffer[pass.to].bind();
          gl.disable(gl.BLEND);
          blurShader.use();
          bindTexture(gl, 0, bloomBuffer[pass.from].texture);
          blurShader.setSampler('SourceTexture', 0);
          blurShader.set('Direction', pass.dx, pass.dy);
          quad.draw();
        }
      }

      //------------------------------------------------------------------
      // 6. The glass, straight onto the canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      // Blending off. This pass writes the layer's whole content, premultiplied
      // and with its own alpha; compositing that is the host's job.
      gl.disable(gl.BLEND);

      glassShader.use();
      bindTexture(gl, 0, phosphorBuffer[target].texture);
      bindTexture(gl, 1, bloomBuffer[0].texture);
      // The source build never samples ClipTexture, but the sampler is still
      // declared and still bound to a unit, and pointing an unused unit at a
      // texture that does exist costs nothing and says nothing.
      bindTexture(gl, 2, phosphorBuffer[target].texture);

      setPreludeUniforms(glassShader, tube.phosphor, graticuleLevel, division);
      glassShader.setSampler('PhosphorTexture', 0);
      glassShader.setSampler('BloomTexture', 1);
      glassShader.setSampler('ClipTexture', 2);

      glassShader.set('OutputSize', width, height);
      glassShader.set('FaceHalf', faceAspect, 1);
      glassShader.set('FaceFit', faceFit);
      glassShader.set('CornerRadius', clamp(tube.cornerRadius, 0, 1));
      glassShader.set('Curvature', CURVATURE);
      glassShader.set('Vignette', VIGNETTE);
      glassShader.set('Halation', HALATION);

      // No perspective control is exposed, and these are set explicitly to zero
      // rather than left unset: an unset uniform is zero on every driver anybody
      // has, and is guaranteed by nobody.
      glassShader.set('PerspectiveX', 0);
      glassShader.set('PerspectiveY', 0);

      glassShader.set('FilterTransmission', FILTER_TRANSMISSION);
      glassShader.set('FaceBlack', FACE_BLACK);
      glassShader.set('Opacity', OPACITY);

      glassShader.set('HasClip', 0);
      glassShader.set('ClipMaxUV', 1, 1);

      quad.draw();

      phosphorIndex = history;
    },
  };
}

//---------------------------------------------------------------------------
// The page.
//---------------------------------------------------------------------------

/// `GetParameterDisplay`'s formatters, so the number beside a slider is the one
/// the plugin would show a host.
const ohms = (r) => (r >= 1e6 ? `${(r / 1e6).toFixed(2)} MR` : `${(r / 1e3).toFixed(1)} kR`);

const seconds = (s) => {
  if (!(s > 0)) return 'off';
  if (s < 1e-3) return `${(s * 1e6).toFixed(0)} us`;
  if (s < 1) return `${(s * 1e3).toFixed(2)} ms`;
  return `${s.toFixed(2)} s`;
};

const farads = (f) => (f < 1e-6 ? `${(f * 1e9).toFixed(1)} nF` : `${(f * 1e6).toFixed(2)} uF`);

/// `%+.2f`, sign always shown — the plugin's own spelling for an offset, where
/// knowing which side of centre you are on is the whole point of the readout.
const signed = (v) => `${v < 0 ? '' : '+'}${v.toFixed(2)}`;

const opt = (id, name, elements, def, group, hint) =>
  ({ id, name, type: 'option', elements, default: def, group, hint });

/// `extra` is either a hint on its own or `{ display, hint }`. Spreading a bare
/// string would give the descriptor a property per character, which the panel
/// then renders as nothing at all.
const std = (id, name, def, group, extra = {}) =>
  ({ id, name, type: 'standard', default: def, group, ...(typeof extra === 'string' ? { hint: extra } : extra) });

const bool = (id, name, def, group, hint) =>
  ({ id, name, type: 'boolean', default: def, group, hint });

//---------------------------------------------------------------------------
// Three of the plugin's readouts need a SIBLING parameter, not just their own
// value: C Fine reports the capacitor its decade multiplies, Mark-Space reports
// a duty that depends on Ra and Rb, and Filter reports a time constant measured
// in this channel's own period. `GetParameterDisplay` gets those by calling
// `Resolve()`, and the kit's `format()` hands a display function its own value
// and nothing else — so the live parameter set is captured here, from what
// `mountDemo` returns, and read back out.
//
// It is filled after the panel has already built itself once, so every readout
// is re-synced below. Before that it falls back to preset row 1, which IS the
// defaults, so even the frame in between is not wrong.
//
// **The sliders, not the preset overlay.** `GetParameterDisplay` feeds Resolve
// the raw values deliberately: the panel is reporting what the control is set
// to, which while a preset is selected is not what the bench is running.
//---------------------------------------------------------------------------
let live = null;
const raw = (id) => (live !== null ? live.get(id) : rowOne(id));

/**
 * One channel's ten controls, in `ChannelControl` order and with the plugin's
 * own names. Built in a loop because the plugin declares them in one, and
 * because six hand-written copies is six chances for channel 4 to disagree with
 * channel 1 about what Mark-Space does.
 *
 * Every default comes from preset row 1 through `rowOne()`, which is the same
 * construction the plugin's constructor uses — so "the defaults" and "preset 1"
 * cannot drift apart here either.
 */
function channelParams(c) {
  const group = `Channel ${c}`;
  const id = (control) => `ch${c}${control}`;
  const at = (control) => rowOne(id(control));

  // This channel's parts, as the panel currently has them.
  const ra = () => resistanceOf(raw(id('ra')));
  const rb = () => resistanceOf(raw(id('rb')));
  const stockDuty = () => (ra() + rb()) / (ra() + 2 * rb());

  return [
    std(id('ra'), `Ch${c} Ra`, at('ra'), group, {
      display: (v) => ohms(resistanceOf(v)),
      hint: '1 kR to 1 MR, logarithmic. Charging current with Rb; raising it lengthens the high time and the period, and raises the stock duty.',
    }),
    std(id('rb'), `Ch${c} Rb`, at('rb'), group, {
      display: (v) => ohms(resistanceOf(v)),
      hint: '1 kR to 1 MR. In the charge path AND the discharge path, which is why the stock duty cannot go below a half and why Mark-Space exists.',
    }),
    opt(id('decade'), `Ch${c} C Decade`, DECADE_NAMES, at('decade'), group,
      'The timing capacitor, as a decade. Period is proportional to it, so a decade here is a decade of frequency.'),
    std(id('fine'), `Ch${c} C Fine`, at('fine'), group, {
      // The capacitor that results, because "x1.03" is only useful next to the
      // decade it multiplies.
      display: (v) => farads(capacitanceOf(raw(id('decade')), v)),
      hint: 'x1 to x10 on the decade. Channel 2 ships at x1.01 — a 1% mismatch, which is what two parts out of the same bag do, and it is why the four dots drift.',
    }),
    opt(id('cv'), `Ch${c} CV Source`, CV_SOURCE_NAMES, at('cv'), group,
      'What drives pin 5, which IS the threshold — so the trigger is half of whatever it says. Audio is in the list because the plugin declares it, and it is a permanent silence here: a browser has no Resolume FFT.'),
    std(id('depth'), `Ch${c} CV Depth`, at('depth'), group, {
      display: (v) => `${(clamp(v, 0, 1) * 100).toFixed(0)}%`,
      hint: 'How hard the source pulls pin 5 off its resting 2/3 Vcc. At full depth it reaches 0.37 Vcc to 0.97 Vcc.',
    }),
    std(id('ms'), `Ch${c} Mark-Space`, at('ms'), group, {
      display: (v) => `${(dutyFor(v, stockDuty()) * 100).toFixed(0)}% high`,
      hint: 'The Maddi pot-and-diode trick: charge through one side of the wiper, discharge through the other, so the total resistance is fixed and only the split moves. Duty goes where you put it and the PERIOD DOES NOT MOVE. Centre is the stock circuit exactly.',
    }),
    bool(id('reset'), `Ch${c} Reset`, at('reset'), group,
      'Pin 4. Off holds pin 3 low and drains the capacitor through Rb, and the trigger comparator cannot set the flip-flop again until it is released. A stopped channel also stops being considered when the engine rate is chosen.'),
    std(id('filter'), `Ch${c} Filter`, at('filter'), group, {
      display: (v) =>
        seconds(
          expoWithOff(v, 0.02, 4)
            * nominalPeriod(ra(), rb(), capacitanceOf(raw(id('decade')), raw(id('fine')))),
        ),
      hint: 'An RC on pin 3, measured in this channel’s own periods rather than in seconds, so it means the same thing on a 2 Hz timer as on a 2 kHz one. It is what turns the dots into lines.',
    }),
    std(id('level'), `Ch${c} Level`, at('level'), group, {
      display: (v) => `${(clamp(v, 0, 1) * 100).toFixed(0)}%`,
      hint: 'An attenuator on everything this channel feeds out — the output, the capacitor and the filtered output alike.',
    }),
  ];
}

const CHANNEL_PARAMS = [];
for (let c = 1; c <= CHANNELS; c += 1) CHANNEL_PARAMS.push(...channelParams(c));

const mounted = mountDemo({
  name: 'Astable',
  pluginId: 'AT01',
  tagline:
    'Six 555 timers, a patch bay and a television’s deflection yoke. Nothing is drawn as a shape: the picture is where the beam went and how long it lingered there, so two square waves into X and Y give four dots because the beam dwells at the rails and crosses between them fast. The tube here is the plugin’s own shaders; the circuit is a full port of its engine — all six timers, solved comparator crossings, the capacitor-coupled yoke and the factory presets. Nothing audio is on this page.',
  repo: 'https://github.com/stoatworks-labs/astable',

  // The stock banner says the page runs "on generated clips". Astable is a
  // SOURCE — SetMinInputs( 0 ), SetMaxInputs( 0 ) — so that would be the banner
  // itself making the kind of claim the banner exists to prevent.
  blurb:
    'It is Astable’s own GLSL, ported from the repository to WebGL2, driven by a JavaScript port of the plugin’s 555 engine — same parameters, same maths, no install. Astable is a source: it reads no video at all, and the picture below is a simulated circuit rather than anything applied to a clip.',

  // The face is the object and the output carries its alpha — `FaceBlack` is 1
  // and the alpha is the face mask — so what sits behind it is a real question.
  // In Resolume that would be the layers below.
  showBackdrop: true,

  // The phosphor buffer is RG32F and the trace pass accumulates into it by
  // additive blending. Both extensions are load-bearing and neither is optional:
  //
  //   EXT_color_buffer_float — without it every phosphor buffer comes back
  //   GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT. Eight bits mid-chain would quantise
  //   the excitation, and the dwell law — which is the whole claim — would be
  //   measuring the quantiser instead.
  //
  //   EXT_float_blend — blending into a 32-bit float target is core on desktop
  //   GL and an extension here. Without it the trace draw is an
  //   INVALID_OPERATION and nothing is deposited at all.
  needFloat: true,
  needFloatBlend: true,

  // Astable has no video input, so there is no clip to pick. Empty rather than a
  // list nothing reads: the two transport controls the kit builds from this are
  // removed from the DOM below.
  sources: [],

  params: [
    ...CHANNEL_PARAMS,

    opt('xSource', 'X Source', PATCH_SOURCE_NAMES, rowOne('xSource'), 'Patch',
      'The patch bay is where the pictures come from. An output is a square between the rails; a capacitor is the exponential between Vcc/3 and 2Vcc/3, so it is a third the size and every edge of the figure becomes a curve; a filtered output is pin 3 through that channel’s RC.'),
    opt('ySource', 'Y Source', PATCH_SOURCE_NAMES, rowOne('ySource'), 'Patch',
      'Two outputs of slightly different period is four dots that drift. Two capacitors at a rational ratio is a Lissajous figure.'),
    opt('zSource', 'Z Source', PATCH_SOURCE_NAMES, rowOne('zSource'), 'Patch',
      'The grid: beam current, not a colour and not an alpha. Read raw rather than through the yoke’s coupling capacitor, because blanking on "pin 3 low" has to mean pin 3.'),
    std('xGain', 'X Gain', rowOne('xGain'), 'Patch', {
      display: (v) => `x${expoWithOff(v, 0.1, 4).toFixed(2)}`,
      hint: 'x0.1 to x4, with a true off over the bottom 2% of the travel.',
    }),
    std('yGain', 'Y Gain', rowOne('yGain'), 'Patch', {
      display: (v) => `x${expoWithOff(v, 0.1, 4).toFixed(2)}`,
    }),
    std('xOffset', 'X Offset', rowOne('xOffset'), 'Patch', {
      display: (v) => signed(linear(v, -1, 1)),
      hint: 'The amplifier’s centring pot, added AFTER the coupling capacitor — which is where a television’s is.',
    }),
    std('yOffset', 'Y Offset', rowOne('yOffset'), 'Patch', {
      display: (v) => signed(linear(v, -1, 1)),
    }),
    opt('zMode', 'Z Mode', Z_MODE_NAMES, rowOne('zMode'), 'Patch',
      'Blank When Low cuts the gun while the source is below zero — which on the Raster preset is the 5% of the cycle that is the line retrace. Brightness maps the source onto beam current instead.'),

    std('coilX', 'Coil X', rowOne('coilX'), 'Yoke', {
      display: (v) => seconds(expoWithOff(v, 10e-6, 100e-3)),
      hint: 'L/R of the horizontal coil. Zero is an electrostatic scope and the crossings are straight lines; anything else is a lag, and a square wave into an inductor is an exponential rather than a step.',
    }),
    std('coilY', 'Coil Y', rowOne('coilY'), 'Yoke', {
      display: (v) => seconds(expoWithOff(v, 10e-6, 100e-3)),
      hint: 'The vertical coil. Different from X on purpose in several presets: a real yoke’s two windings are not the same part.',
    }),
    std('deflectionGain', 'Deflection Gain', rowOne('deflectionGain'), 'Yoke', {
      display: (v) => `x${linear(v, 0, 2).toFixed(2)}`,
      hint: 'The amplifier’s gain, ahead of the rail. Turning it up past the rail clips rather than growing the figure.',
    }),
    std('ampRail', 'Amp Rail', rowOne('ampRail'), 'Yoke', {
      display: (v) => `${expo(v, 0.2, 3).toFixed(2)} V`,
      hint: 'Deflection volts the amplifier runs out at. Below the signal it flattens the figure against the rails, which is what an overdriven deflection stage does.',
    }),

    std('vcc', 'Vcc', 0.4, 'Supply', {
      display: (v) => `${linear(v, 5, 15).toFixed(1)} V`,
      hint: 'The bench supply, shared by all six. It sets the output swing and the comparator levels, and the period is independent of it — which is the 555’s whole trick, and something you can watch here by turning it while the figure does not change size.',
    }),

    opt('phosphor', 'Phosphor', PHOSPHOR_NAMES, 0, 'Tube',
      'A measured table, not a tint. P4 is the monochrome television white and the default, because this tube is a portable telly. At persistence x1 it shows no trail at 60 fps, because both its decay constants are tens of microseconds — which is what a P4 is. Switching changes the brightness by up to three times, because the efficiencies are real.'),
    std('persistence', 'Persistence', PERSISTENCE_UNITY, 'Tube', {
      display: (v) => `x${persistenceMultiplier(v).toFixed(persistenceMultiplier(v) < 10 ? 2 : 0)}`,
      hint: 'A multiplier on the phosphor’s own time constants, not a per-frame decay: a decay factor would mean something different at every frame rate. The default is x1 — the real phosphor. Turn it up to get what a camera’s open shutter got.',
    }),
    std('focus', 'Focus', 0.40, 'Tube', {
      display: (v) => `σ ${expo(v, 0.0012, 0.02).toFixed(4)}`,
      hint: 'Beam units, one of which is half the face height. The face buffer is sized by this rather than by the composition, so a sharper tube genuinely costs more — and it moves in rungs, because reallocating the buffer throws the phosphor history away.',
    }),
    std('faceAspect', 'Face Aspect', 0.415, 'Tube', {
      display: (v) => `${expo(v, 1, 2).toFixed(2)}:1`,
      hint: '4:3 by default: a television. The only thing that knows the face is not square, and it enters the trace shader as a single divide — so the spot stays round rather than turning into an ellipse.',
    }),
    std('cornerRadius', 'Corner Radius', 0.18, 'Tube',
      'Rounded-rectangle distance field in the tube’s own coordinates. Radius 1 on a square face already IS a circle, which is why a lab scope and a television are the same geometry with different numbers rather than two code paths.'),
    std('overscan', 'Overscan', 0.5625, 'Tube', {
      display: (v) => `${(linear(v, 0.6, 1.4) * 100).toFixed(0)}%`,
      hint: 'Beam units per volt. Over 100% scans a raster larger than the face, which is how a television keeps its blanking edges behind the bezel.',
    }),
    std('brightness', 'Brightness', rowOne('brightness'), 'Tube', {
      display: (v) => `${(Math.pow(10, -1 + 2 * clamp(v, 0, 1)) * Math.min(1, v * 50)).toFixed(2)}x`,
      hint: 'Energy per second of beam-on time, exponential around a calibrated 1.0, with a true zero over the bottom 2% of the travel. It is a preset column rather than a fixed default, because a figure that parks the beam and one that sweeps it differ in peak brightness by two orders of magnitude.',
    }),

    opt('preset', 'Preset', PRESET_NAMES, 0, 'Preset',
      'An OVERRIDE, not a write. While this is on anything but Custom the row’s values are laid over the whole bench every frame at read time, and the sliders above are — for those columns — not the truth. That is the plugin’s behaviour and the reason for it is the host: Resolume does not consume value events, so a plugin cannot push values back into the inspector. Put it on Custom to get the sliders back.'),
  ],

  differences: [
    'Nothing audio. The plugin declares an Audio FFT buffer parameter that Resolume fills, an Audio Gain over it, and an Audio element in every channel’s CV Source. There is no Resolume FFT in a browser, and asking a visitor for a microphone to demonstrate a video source is not a trade worth making — so the buffer parameter and Audio Gain are absent from this panel, and choosing Audio as a CV source here is choosing a permanent silence.',
    'There is no clip, and no "use my own file". Astable is a source: it declares zero inputs and the glass shader’s HasClip is 0, so ClipTexture is never read. The kit offers both controls to every demo and this page removes them, rather than leaving a control present and inert.',
    'The renderer is not a subset. All six passes are the plugin’s own GLSL in the plugin’s own order, and demo/tools/check_shaders.py fails the repository’s verify script if a character of it drifts from source/render/shaders/. The settings the plugin fixes in code — blooming, halation and its knee, curvature, vignette, and the absence of a graticule or a contrast filter — are fixed here too, at the plugin’s own values, because a portable television has no knobs for them.',
    'The engine is not a subset either: all six timers, the patch bay, the capacitor-coupled yoke and the seven factory presets, at the plugin’s own rate — 96 samples of the fastest running channel’s period, clamped to 24 to 384 kHz. What is absent is the plugin’s host plumbing: the About block, the FFGL parameter conversion, and the Diag log.',
    'The clock is the browser’s. Frame durations are clamped to the same 1/240 to 1/24 window the plugin clamps its host’s to, so a stalled tab cannot deposit half a second of beam in one frame — but a browser’s timing is not a host’s, and brightness is only independent of it because dt is carried per sample. Restart is taken as a power cycle rather than as a very long frame, because time running backwards is the one thing a deflection amplifier cannot do.',
    'The engine runs on the main thread, at the plugin’s rate. A slower machine drops frames rather than lowering the rate, and a dropped frame is a longer frame, which is more samples — so what gives way is the frame rate and not the circuit. On a machine that cannot keep up the figure is still right; it just arrives less often.',
    'Nothing here is measured. The plugin’s numerical proof — the period against 0.693 (Ra + 2Rb) C within 1%, the capacitor’s swing against pin 5, Mark-Space moving duty while holding period within 0.5%, more than 90% of the light landing in four spots, a coil’s fitted time constant within 1%, and total light independent of sweep speed within 0.5% — is tools/attest in the repository, and that harness, not this page, is the reason to believe the model.',
    'The plugin has never been run on a GPU inside Resolume. It registered, loaded and drew in Arena 7.27.1 on Windows on Mesa llvmpipe, and every timing figure quoted anywhere is macOS. This page is a browser and is not evidence about either.',
  ],

  createRenderer,
});

// The panel built itself once already, before this existed, so every readout
// that reads a sibling is re-synced. `reset` is the event `buildPanel` listens
// to for a whole-panel refresh; it carries no values and changes none.
live = mounted?.params ?? null;
live?.dispatchEvent(new CustomEvent('reset'));

//---------------------------------------------------------------------------
// The two controls a source has no use for.
//
// `demo.sources` is empty, so the kit builds a Clip dropdown with no entries and
// a "Use my own…" file picker that would load a texture nothing samples. Both
// are removed here rather than left in place: the rule this kit is built on is
// that a demo says what it cannot do, and a control that is present and dead is
// a worse answer than one that is absent.
//
// Done from the page rather than by teaching the kit about sourceless demos,
// because the kit is vendored into eighteen repos by sync.sh and this is the
// first plugin in it with no input at all. If a second one arrives, that is the
// moment to move this upstream.
//---------------------------------------------------------------------------
for (const field of document.querySelectorAll('.transport__field')) {
  if (field.querySelector('.transport__label')?.textContent === 'Clip') field.remove();
}
document.querySelector('.transport__file')?.remove();
