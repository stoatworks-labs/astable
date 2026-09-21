#pragma once

#include "Controls.h"
#include "render/BeamGeometry.h"
#include "signal/Bench.h"
#include "signal/Clock.h"

#include <FFGLSDK.h>

#include <string>

namespace astable
{
/**
	Six 555 timers on a breadboard, patched into the X, Y and brightness of a
	television's deflection yoke.

	## The one idea

	**The picture is where the beam went.** Nothing here draws a shape. Six
	timers are simulated at the component level -- a capacitor, two
	comparators, a flip-flop -- and whatever their outputs and capacitors are
	doing is what the yoke is fed. Four dots, lines, curves, the crawl, the
	raster: all of them are consequences of that, and the harness measures the
	consequences rather than asserting the shapes.

	The renderer is vectrix's, copied: it deposits a fixed quantum of energy
	per sample interval and spreads it over the distance the beam covered, so
	a beam that dwells is bright and one that crosses fast is faint, with no
	1/v anywhere. That is what makes two square waves come out as four dots.

	## Shape of a frame

	    ProcessOpenGL:
	      clock.Update(hostTime)        ms/seconds auto-detect; dt clamped
	      updateAudio()                 the host's spectrum, folded to a level
	      Resolve(effective params)     0..1 -> ohms, farads, seconds
	      bench.SetParams(); rate = bench.SampleRate()
	      n = clock.SamplesForThisFrame(rate)
	      bench.Render(n)               six timers -> patch -> yoke -> samples
	      beam.Render(samples)          decay -> trace -> halation -> glass
*/
class AstablePlugin : public CFFGLPlugin
{
public:
	AstablePlugin();
	~AstablePlugin() override = default;

	FFResult InitGL( const FFGLViewportStruct* viewPort ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// What a control's value MEANS, in the units it is really in. Resolume
	/// shows this instead of the raw 0..1. Resolved on demand, never cached:
	/// Arena asks as it applies a new value, before that value reaches the
	/// render thread.
	char* GetParameterDisplay( unsigned int index ) override;

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	//--- for the offline harness --------------------------------------------

	/// Declare the host clock's unit instead of letting Clock infer it.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// Fill the spectrum the host would have sent, so the audio path can be
	/// exercised without a host. `level` is what updateAudio should fold it
	/// to, 0..1. The bins are protected SDK state, which is why this is a
	/// member and not something the harness pokes from outside.
	void SetAudioForTest( float level );

	/// The value a parameter is actually rendered with: the preset's, while one
	/// is selected and it covers the parameter; the slider's otherwise.
	float Effective( unsigned int index ) const;

	/// The bench, for the harness to read timers off.
	const Bench& TheBench() const
	{
		return bench;
	}

private:
	void declareParameters();
	void updateAudio();
	Resolved resolve() const;
	BeamGeometry::RenderParams renderParams( const TubeSettings& tube, double frameSeconds ) const;

	float params[ PT_COUNT ] = {};

	Clock clock;
	Bench bench;
	BeamGeometry beam;

	/// The host's spectrum, folded to one level and released slowly.
	float audioLevel   = 0.0f;
	double audioClock  = -1.0;
	bool audioInjected = false;

	/// The host is handed a bare pointer for the display string and the About
	/// block, so both strings outlive the call that built them.
	std::string displayValue;
	std::string aboutText;

	bool glReady        = false;
	long frameCounter   = 0;
	double lastHostTime = -1.0;
};

} // namespace astable
