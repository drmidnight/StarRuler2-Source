# Star Ruler 2 for Apple Silicon Macs

### Disclaimer: This is heavily claude generated. I don't have the time to do this all myself, but I love this game and want a smooth experience on my macbook.

This is a fork of [BlindMindStudios/StarRuler2-Source](https://github.com/BlindMindStudios/StarRuler2-Source)
that runs Star Ruler 2 natively on Apple Silicon Macs (macOS 13 or later). It upgrades the
scripting engine to AngelScript 2.37 for native arm64 support, renders with Metal instead of
OpenGL, fixes the macOS window and input problems, and packages the game as a Mac app. Download the disk image from the
[Releases](https://github.com/drmidnight/StarRuler2-Source/releases) page, or see
[Building on macOS](#building-on-macos) below.

This fork only targets Apple Silicon Macs, and the Windows and Linux build instructions below
are from the original project: without OpenGL those builds have no renderer. If something
doesn't work, please
[open an issue](https://github.com/drmidnight/StarRuler2-Source/issues) describing what
happened, and attach `~/.starruler2/log.txt` if the game crashed.

## Changes
```
  Platform and build
  - Builds and runs natively on Apple Silicon (macOS 13 or later), compiled as C++20.
  - AngelScript upgraded to 2.37.0, with the game scripts and add-ons adapted.
  - Dependencies are built as static libraries, so no Homebrew is needed. The dependencies include GLFW 3.4,
    glslang and SPIRV-Cross.
  - Packaged as an ad-hoc signed .app and disk image, with a dSYM for reading crash logs.
  - Thread and address sanitizer builds, Instruments signposts, and threading tests.

  Rendering
  - A Metal renderer, now the default, with OpenGL 4.1 core as the alternative. You choose between them in
    the options or with --render-backend.
  - The renderer-independent code moved into a shared BaseDriver.
  - Old GL shaders are translated to GLSL 4.10 and Metal, and a shader stands in for the old fixed-function
    pipeline.
  - Rendering at Retina resolution, with fonts drawn at the screen's pixel density.
  - On Metal, textures load on background threads, and frames in flight are capped at 3.
  - A --capture mode and a comparison tool check that Metal renders the same as GL.

  Threading and performance
  - New threading primitives, plus a worker pool with priority lanes that drives the simulation.
  - Threads are named and run at the right priority.
  - Waits block instead of spinning: frame pacing, script calls to objects and empires, and script-thread
    pauses.
  - Data races fixed: atomic flags, async call returns, vision sharing, the threaded loader and script
    garbage collection.
  - The frame limiter supports refresh rates above 60 Hz. The galaxy view now holds a steady 120 fps.

  Window and input
  - GLFW 3.4, with borderless fullscreen above the menu bar and Cmd+Enter to toggle it. The fullscreen
    option applies without a restart.
  - Trackpad: smooth scroll-zoom, pinch to zoom, and an option to scroll to pan.
  - Gamepad support through the window driver, and a fix for the backtick key.

  Fixes
  - Mod folder paths, symlink scanning, monotonic game timing, a --test-scripts failure exit, and script
    division by zero.
```
---

# Star Ruler 2
Star Ruler 2 is a massive scale 4X/RTS set in space. Explore dozens, hundreds,
or even thousands of systems in a galaxy of your choosing, expand across its
planets, exploit the resources you find, and ultimately exterminate any who
stand in your way. The fate of your empire depends on your ability to master
the economy, field a military, influence galactic politics, and learn what you
can about the universe.

## What is this?
This repository contains the full source code needed to build Star Ruler 2, and
all secondary scripts, data files and assets required to run it.

Blind Mind Studios has been inactive for a few years now, so we decided to
open-source the game as a whole and allow anyone interested in how its engine works
to tinker with it.

## Building the Game
The Star Ruler 2.exe contained in the main folder is just a launcher. To run
the game you will need to build the binary for your platform from source.

### Building on Windows
You will need Visual Studio 2017 to build Star Ruler 2 on windows. Simply open the
visual studio solution in source/msvc/Star Ruler 2/, and build the "Star Ruler 2" project
in either Debug or Non-Steam Release configuration.

Please note that while the built exe ends up in the bin/win64/ directory, it expects to be
started with its working directory set to the main directory (where the
launcher exe is). So when debugging from visual studio, make sure the 'Working
Directory' configuration property is set to ../../..

### Building on Linux
The equivalent launcher on linux is StarRuler2.sh. To build the binary files on linux,
make sure your working directory is set to the main directory (the one with StarRuler2.sh in it),
then run `make -f source/linux/Makefile compile` to compile the binary.

Afterward, run the `StarRuler2.sh` shell script to start the game.

Several dependencies are required to build on linux, including libpng, zlib,
GLEW, GLU, freetype2, libvorbisfile, libvorbis, libogg, libopenal, libbz2,
libXRandR, and libcurl.


### Building on macOS
Apple Silicon Macs running macOS 13 or later are supported. Install the Xcode command line
tools and CMake, then from the main directory:

    bash source/macos/build_deps.sh
    make -f source/linux/Makefile compile
    bash source/macos/make_app.sh

`build_deps.sh` builds libpng, FreeType, libogg, libvorbis, GLFW, glslang, SPIRV-Cross and
astc-encoder as static libraries in `deps/macos`, so the game has no Homebrew dependencies. `make_app.sh` creates an ad-hoc signed
`dist/Star Ruler 2.app` and `dist/StarRuler2-<version>-macos-arm64.dmg`. Without
`deps/macos` the makefile falls back to Homebrew's libraries, which is fine for running the game
locally with `StarRuler2.sh`.

GitHub Actions runs the same steps on every push to `master` (`.github/workflows/macos.yml`) and
keeps the disk image as a build artifact. Pushing a tag starting with `v` also publishes it as a
release.

## Differences with Commercial Version
* The music is not part of the open source release of Star Ruler 2. The data/music/ directory from the commercial
  release can be copied directly into the data/ directory of an open-source build. The music will be detected and played.
* All code for accessing the SteamWorks API has been removed. The game platform abstraction layer game\_platform.h has 
  been kept intact, so other mod-sharing platforms could potentially be integrated into the game.
* Code for the automatic game patcher that is part of the GOG.com release is part of the repository, but the functionality
  is disabled, since it relies on delta updates and will not work with modified open source versions.
* While the actual source code is probably still compatible with 32-bit platforms, project files and external dependencies for
  32-bit builds are not included in this repository.

## FAQ
### Can I Play Multiplayer with Steam/GOG versions?
Yes. The base open source version is multiplayer-compatible with the commercial versions. If you make multiplayer-incompatible
changes to the open source version, please remember to change the MP\_VERSION identifier in scripts/definitions/version.as,
so things do not break from people with incompatible versions trying to join each other.

### Does this have the Wake of the Heralds DLC Expansion?
Yes. The code has been modified to bypass all DLC checks and unlock it by default. The Wake of the Heralds content
is always available in the open-source version.

### Is there a Community?
Most Star Ruler 2 community activity is centered around the [Steam Forums](https://steamcommunity.com/app/282590/discussions/)
(it is possible to post without owning the game on steam), and the [Discord Server for Rising Stars](https://discord.gg/9YyXgbh),
one of the largest SR2 mod projects.

## Licensing
Star Ruler 2 source code is licensed as MIT, art assets are licensed as CC-BY-NC 2.0.

Some third party code is contained in this repository as dependencies. Licenses for those projects are contained in the appropriate source folders.

See the COPYING file for more information.
