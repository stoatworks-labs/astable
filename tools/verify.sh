#!/usr/bin/env bash
#
# Everything that can be checked without a human, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Half of this file checks things the RELEASE job checks. That is deliberate,
# and it is the fleet's most expensive lesson: a check that only ever runs in
# CI, after a tag, is a check that will catch you after the tag -- and the fix
# for a bad tag is to re-point it, which strands the release unsigned for ever
# unless the autosign state file is edited by hand.
#
# The two that have actually bitten this fleet:
#
#   * `CFBundleExecutable` carrying the PREVIOUS plugin's name, because the
#     plist template was copied from another repo. Nothing fails: the bundle
#     assembles, the binary is universal, `nm` finds the entry point and a
#     probe renders a correct frame. Then codesign says "code object is not
#     signed at all" and mentions nothing about a plist.
#
#   * A macOS build that is quietly arm64-only, because CMAKE_OSX_ARCHITECTURES
#     was set after the first target existed. The build log calls that a
#     success. Only `lipo` knows.
#
# And the one only oxbow can see: the plugin registers itself from a file-scope
# constructor nothing references, so a bundle can load, export plugMain, and
# report that it contains no plugins. A clean build and a green test run do not
# establish that it registers. `oxbow selftest` does.
#
set -uo pipefail

cd "$(dirname "$0")/.."

PASS=0
FAIL=0

ok()    { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()   { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
    local dir bad=0 n=0 shader

    if ! command -v glslc >/dev/null 2>&1; then
        printf '   skipped: glslc not installed (brew install shaderc)\n'
        return 0
    fi

    dir="$( mktemp -d )"

    python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/render/shaders/Prelude.cpp",
	"source/render/shaders/Trace.cpp",
	"source/render/shaders/Decay.cpp",
	"source/render/shaders/Bloom.cpp",
	"source/render/shaders/Glass.cpp",
]

# Shaders the plugin assembles at run time.
# Mirrors vertexSource()/fragmentSource() in Prelude.cpp and their call sites.
ASSEMBLED = {
	"screenVertex":   [ "kConstants", "kScreenVertexBody" ],
	"traceVertex":    [ "kConstants", "kTraceVertexBody" ],
	"traceFragment":  [ "kConstants", "kFragmentHelpers", "kTraceFragmentBody" ],
	"decayFragment":  [ "kConstants", "kFragmentHelpers", "kDecayFragmentBody" ],
	"brightFragment": [ "kConstants", "kFragmentHelpers", "kBrightFragmentBody" ],
	"blurFragment":   [ "kConstants", "kFragmentHelpers", "kBlurFragmentBody" ],
	"glassFragment":  [ "kConstants", "kFragmentHelpers", "kGlassFragmentBody" ],
}

# A shader may be several adjacent raw strings -- MSVC caps one literal at
# about 16 KB -- so everything up to the terminating semicolon is joined.
named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*(?:\[\s*\])?\s*=\s*((?:\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )
	for m in re.finditer( r'(?<![\w)]\s)R"\((.*?)\)"', text, re.S ):
		pass

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# A literal starts with #version; anything else names a constant above --
	# and a name that has moved is a KeyError here, not a silent skip.
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

    for shader in "$dir"/*.vert "$dir"/*.frag; do
        [ -e "$shader" ] || continue
        n=$(( n + 1 ))
        if ! glslc --target-env=opengl4.5 -fauto-map-locations \
               "$shader" -o /dev/null 2>"$dir/err"; then
            printf '   %s does not compile\n' "$( basename "$shader" )"
            sed "s|$dir/||; s|^|      |" "$dir/err"
            bad=$(( bad + 1 ))
        fi
    done

    if [ "$n" -eq 0 ]; then
        # No shaders at all is a FAILURE, not a pass. It means the extraction
        # above has lost track of where this repo keeps its GLSL, and a check
        # that silently looks at nothing is worse than no check.
        printf '   no shaders were extracted -- the extraction has gone stale\n'
        rm -rf "$dir"
        return 1
    fi

    if [ "$bad" -eq 0 ]; then
        printf '   %d shaders, all compile\n' "$n"
    fi
    rm -rf "$dir"
    return "$bad"
}

#---------------------------------------------------------------------------
head_ "Shaders"
#---------------------------------------------------------------------------
if shaders_compile; then
    ok "every shader compiles"
else
    bad "a shader does not compile"
fi

#---------------------------------------------------------------------------
head_ "Build (fresh, universal, plugin + harness)"
#---------------------------------------------------------------------------
# The build directory is DELETED first, and that is not belt and braces.
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is
# exactly where the architecture list lives. A developer who configured once
# with `-DCMAKE_OSX_ARCHITECTURES=arm64` for a fast iteration loop -- which is
# the documented way to work in CLAUDE.md -- leaves a tree where this script
# happily rebuilds, finds a single-architecture binary, and reports it as a
# defect in the source.
rm -rf build

if cmake -B build -DCMAKE_BUILD_TYPE=Release >/tmp/astable-configure.log 2>&1 \
   && cmake --build build -j8 >/tmp/astable-build.log 2>&1; then
    ok "configured and built"
else
    bad "build failed -- see /tmp/astable-build.log"
    tail -25 /tmp/astable-build.log
    exit 1
fi

BUNDLE="build/Astable.bundle"
BIN="$BUNDLE/Contents/MacOS/Astable"

#---------------------------------------------------------------------------
head_ "Architectures"
#---------------------------------------------------------------------------
# lipo, never the build log. A single-architecture build is a successful build.
if [ ! -f "$BIN" ]; then
    bad "missing: $BIN"
else
    archs="$(lipo -archs "$BIN" 2>/dev/null)"
    case "$archs" in
        *x86_64*arm64*|*arm64*x86_64*) ok "universal: Astable ($archs)" ;;
        *) bad "NOT universal: Astable ($archs)" ;;
    esac
fi

#---------------------------------------------------------------------------
head_ "Entry point"
#---------------------------------------------------------------------------
# The symbol table is captured and matched with `case`, so there is NO PIPELINE
# at any point. That is not a style choice.
#
# `nm -gU "$bin" | grep -q _plugMain` under `set -o pipefail` fails BECAUSE the
# symbol was found: `grep -q` exits at the first match, `nm` takes SIGPIPE, and
# pipefail propagates it. It is a race against how fast the producer finishes,
# so it bites the biggest artefact first and looks intermittent.
symbols_of() {
    nm -gU "$1" 2>/dev/null || true
}

case "$(symbols_of "$BIN")" in
    *_plugMain*) ok "exports plugMain" ;;
    *) bad "no plugMain -- the bundle contains no plugin" ;;
esac

#---------------------------------------------------------------------------
head_ "Bundle layout and signing"
#---------------------------------------------------------------------------
plist_exec="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
              "$BUNDLE/Contents/Info.plist" 2>/dev/null)"
if [ "$plist_exec" = "Astable" ]; then
    ok "CFBundleExecutable is Astable"
else
    bad "CFBundleExecutable is '$plist_exec', not Astable"
fi

if [ -f "$BUNDLE/Contents/MacOS/$plist_exec" ]; then
    ok "CFBundleExecutable matches a real binary on disk"
else
    bad "CFBundleExecutable names a file that is not there"
fi

plist_id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' \
            "$BUNDLE/Contents/Info.plist" 2>/dev/null)"
if [ "$plist_id" = "com.stoatworks.ffgl.astable" ]; then
    ok "CFBundleIdentifier is com.stoatworks.ffgl.astable"
else
    bad "CFBundleIdentifier is '$plist_id'"
fi

# The exact command the release job runs, on a COPY, where it costs a second
# instead of a failed tag.
signdir="$(mktemp -d)"
cp -R "$BUNDLE" "$signdir/" 2>/dev/null
if codesign --force --sign - --timestamp=none "$signdir/Astable.bundle" >/dev/null 2>&1; then
    ok "ad-hoc codesign succeeds (the command the release job runs)"
else
    bad "codesign fails -- this is the failure that never mentions the plist"
fi
rm -rf "$signdir"

#---------------------------------------------------------------------------
head_ "The circuit, and the picture"
#---------------------------------------------------------------------------
# These are the point of the harness: they turn "this is a 555, not a square
# wave generator" from a sentence in AGENTS.md into something a machine checks.
if [ -x build/attest ]; then
    for test in period swing markspace yoke dots energy presets defaults names; do
        log="/tmp/astable-$test.log"
        if ./build/attest "--$test" >"$log" 2>&1; then
            ok "attest --$test"
        else
            bad "attest --$test -- see $log"
            tail -12 "$log"
        fi
    done
else
    bad "attest was not built"
fi

#---------------------------------------------------------------------------
head_ "The browser demo"
#---------------------------------------------------------------------------
# `demo/plugin.js` carries nine GLSL fragments and so does
# `source/render/shaders/`. That is two copies of the same text, and two copies
# drift — quietly, because a demo that renders a PLAUSIBLE picture looks exactly
# like one that renders the right one. The page's whole claim is that it runs the
# plugin's own shaders rather than something reimplemented to look similar, so
# the claim needs something enforcing it, and nothing else can: `attest` drives
# the real plugin class through the real FFGL sequence and has no idea the page
# exists.
if [ -f demo/tools/check_shaders.py ]; then
    if out="$( python3 demo/tools/check_shaders.py 2>&1 )"; then
        ok "$( printf '%s' "$out" | tail -1 )"
    else
        bad "the demo's shader copies have drifted from source/render/shaders/"
        printf '%s\n' "$out" | tail -12
    fi
else
    printf '   skipped: demo/tools/check_shaders.py is not present\n'
fi

#---------------------------------------------------------------------------
head_ "Controls"
#---------------------------------------------------------------------------
# A GLSL uniform name that does not match the C++ is silently ignored --
# glGetUniformLocation returns -1 and glUniform(-1) is a documented no-op -- so
# a control can be stone dead while everything compiles, links, loads and
# renders. Nothing else catches it.
if python3 tools/sweep.py >/tmp/astable-sweep.log 2>&1; then
    ok "$(tail -1 /tmp/astable-sweep.log)"
else
    bad "dead controls -- see /tmp/astable-sweep.log"
    tail -20 /tmp/astable-sweep.log
fi

#---------------------------------------------------------------------------
head_ "In a host"
#---------------------------------------------------------------------------
# The only check here that proves the bundle actually REGISTERS a plugin. A
# clean build, a green harness and an exported plugMain do not: CFFGLPluginInfo
# is a file-scope constructor nothing references by name.
OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
if [ -x "$OXBOW" ]; then
    out="$("$OXBOW" probe "$BUNDLE" 2>&1)"
    case "$out" in
        *"SW Astable"*) ok "oxbow reads the name as SW Astable" ;;
        *) bad "oxbow does not report the name SW Astable" ;;
    esac
    case "$out" in
        *"id:          AT01"*) ok "the FFGL id is AT01" ;;
        *) bad "the FFGL id is not AT01" ;;
    esac
    case "$out" in
        *"type:        source"*) ok "the plugin type is source" ;;
        *) bad "the plugin type is not source" ;;
    esac

    # A source that draws with no input and no file, so a PASS is meaningful
    # here rather than merely "it did not crash".
    out="$("$OXBOW" selftest "$BUNDLE" 2>&1)"
    case "$out" in
        *"FF_INSTANTIATE_GL failed"*) bad "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
        *"selftest:    PASS"*) ok "registers, instantiates and lights pixels ($(printf '%s' "$out" | sed -n 's/^lit pixels:  //p'))" ;;
        *) bad "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
    esac
else
    printf '   skipped: oxbow not built at %s\n' "$OXBOW"
fi

#---------------------------------------------------------------------------
head_ "Cost"
#---------------------------------------------------------------------------
# Not pass/fail -- there is no threshold worth asserting on somebody else's
# GPU -- but a verify run leaves a timing on the record, which is what turns
# "it feels slower" into a comparison.
if [ -x build/attest ]; then
    ./build/attest --bench 2>&1 | sed -n '3,6p'
fi

#---------------------------------------------------------------------------
printf '\n\033[1m%d passed, %d failed\033[0m\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
