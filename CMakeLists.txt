cmake_minimum_required(VERSION 3.15)
project(Astable VERSION 0.1.0 LANGUAGES CXX)

#-----------------------------------------------------------------------------
# Architecture. Must sit immediately after project() and before any target
# exists: CMake latches the architecture list when the first target is created,
# and setting it later is silently ignored -- the build succeeds and produces a
# single-architecture binary. Verify the result with `lipo`, never with the log.
#-----------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()
# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(ASTABLE_BUILD_TOOLS "Build the offline render and measurement harness" ON)

#-----------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins.
#-----------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

#-----------------------------------------------------------------------------
# Everything the plugin shares, as an OBJECT library.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor and nothing ever references that object by name, so in
# an archive the linker is entitled to drop the whole translation unit -- and
# the result is a bundle that loads, exports plugMain, and reports that it
# contains no plugins.
#
# Note what is NOT in here: SourcePlugin.cpp. It carries the CFFGLPluginInfo
# and is listed directly in its own MODULE target below, so the harness -- which
# links this library too -- does not carry a registration it cannot use.
#-----------------------------------------------------------------------------
add_library(astable_core OBJECT
    source/Astable.h                source/Astable.cpp
    source/Controls.h               source/Controls.cpp
    source/Presets.h
    source/Diag.h                   source/Diag.cpp
    source/StoatworksAbout.h
    source/StoatworksAboutLinks.h
    source/StoatworksAboutParams.h

    source/signal/Signal.h
    source/signal/Clock.h           source/signal/Clock.cpp
    source/signal/Timer555.h        source/signal/Timer555.cpp
    source/signal/Bench.h           source/signal/Bench.cpp

    # vectrix's renderer, copied. See ATTRIBUTIONS.md.
    source/render/ScopeBuffer.h     source/render/ScopeBuffer.cpp
    source/render/GLState.h
    source/render/Phosphor.h        source/render/Phosphor.cpp
    source/render/Tube.h            source/render/Tube.cpp
    source/render/BeamGeometry.h    source/render/BeamGeometry.cpp
    source/render/Shaders.h
    source/render/shaders/Prelude.cpp
    source/render/shaders/Trace.cpp
    source/render/shaders/Decay.cpp
    source/render/shaders/Bloom.cpp
    source/render/shaders/Glass.cpp
)

target_include_directories(astable_core PUBLIC source)
target_link_libraries(astable_core PUBLIC ffgl::sdk)
target_compile_definitions(astable_core PUBLIC ASTABLE_VERSION="${PROJECT_VERSION}")
set_target_properties(astable_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers pull in <GL/glew.h>. The SDK links GLEW
# privately, so consumers need it too -- for the include path and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this. GLEW arrives through vcpkg.json,
# which nothing in this file points at: lose it and only Windows breaks, and
# only at configure time.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(astable_core PUBLIC GLEW::GLEW)
endif()

#-----------------------------------------------------------------------------
# The plugin. FFGL resolves one plugMain per binary.
#-----------------------------------------------------------------------------
function(astable_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    target_link_libraries(${target} PRIVATE astable_core)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL" "-framework Carbon" "-framework AppKit")
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

astable_add_plugin(AstableSource source/SourcePlugin.cpp
    "Astable" "com.stoatworks.ffgl.astable")

#-----------------------------------------------------------------------------
# Install: straight into Resolume's plugin folder. Avenue reads
# ~/Documents/Resolume Avenue/Extra Effects instead; pass --prefix for that.
#-----------------------------------------------------------------------------
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

install(TARGETS AstableSource
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

#-----------------------------------------------------------------------------
# The offline harness. macOS only, one translation unit, and it links the same
# OBJECT library the bundle does -- so it measures the code that ships.
#-----------------------------------------------------------------------------
if(ASTABLE_BUILD_TOOLS AND APPLE)
    add_executable(attest tools/attest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines in
    # the harness rather than a vendored dependency.
    target_link_libraries(attest PRIVATE astable_core z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()
