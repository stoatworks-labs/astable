/**
	The FF_SOURCE registration.

	**This file is listed directly in the AstableSource target, not in
	astable_core.** It is also why the core is an OBJECT library rather than a
	STATIC one: `CFFGLPluginInfo` registers itself from a file-scope
	constructor and nothing ever references it by name, so in an archive the
	linker is entitled to drop the whole translation unit -- giving a bundle
	that loads, exports `plugMain`, and reports that it contains no plugins.

	    nm -gU Astable.bundle/Contents/MacOS/Astable | grep plugMain
*/
#include "Astable.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< astable::AstablePlugin >,                 // Create method
	"AT01",                                                  // Plugin unique ID of maximum length 4
	"SW Astable",                                            // Plugin name: 16 characters, not null-terminated
	2,                                                       // API major version number
	1,                                                       // API minor version number
	0,                                                       // Plugin major version number
	1,                                                       // Plugin minor version number
	FF_SOURCE,                                               // Plugin type
	"Six 555 timers on a breadboard, patched into the X, Y and brightness of a television's deflection yoke.\n\nNothing is drawn as a shape. Each timer is simulated at the component level -- a capacitor, two comparators, a flip-flop -- and the picture is where the beam went. Two square waves give four dots; an RC on an output turns them into lines; the capacitors give curves; one timer's cap into another's pin 5 is FM; and because nothing is phase-locked, the figure crawls at the beat between the timers.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Astable FFGL source"                                    // About
);

extern "C" const char* AstableSourceBuildStamp()
{
	return "astable " ASTABLE_VERSION " source, built " __DATE__ " " __TIME__;
}
