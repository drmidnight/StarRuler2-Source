#!/bin/bash
#Builds the third party libraries the game links on macOS as static libraries
#targeting MACOSX_DEPLOYMENT_TARGET, so the app runs on Macs without Homebrew.
#Usage, from the repository root: bash source/macos/build_deps.sh
set -euo pipefail

export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"
ROOT="$(pwd)"
PREFIX="$ROOT/deps/macos"
WORK="$ROOT/deps/build"
JOBS="$(sysctl -n hw.ncpu)"
export CC=/usr/bin/clang CXX=/usr/bin/clang++
CMAKE_COMMON=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX"
	-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
	-DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_FIND_FRAMEWORK=LAST)

mkdir -p "$PREFIX" "$WORK"

#name url sha256
fetch() {
	local name="$1" url="$2" sha="$3" file="$WORK/$1-$(basename "$2")"
	if [[ ! -f "$file" ]]; then
		curl -fL --retry 3 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	echo "$sha  $file" | shasum -a 256 -c - >/dev/null || { echo "Checksum mismatch for $file" >&2; exit 1; }
	rm -rf "$WORK/$name"
	mkdir -p "$WORK/$name"
	tar -xf "$file" -C "$WORK/$name" --strip-components=1
}

cmake_build() {
	local name="$1"; shift
	cmake -S "$WORK/$name" -B "$WORK/$name/_build" "${CMAKE_COMMON[@]}" "$@" >/dev/null
	cmake --build "$WORK/$name/_build" -j "$JOBS" >/dev/null
	cmake --install "$WORK/$name/_build" >/dev/null
	echo "built $name"
}

fetch libogg https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz \
	c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705
cmake_build libogg -DINSTALL_DOCS=OFF

fetch libvorbis https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz \
	b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b
cmake_build libvorbis

fetch libpng https://download.sourceforge.net/libpng/libpng-1.6.43.tar.xz \
	6a5ca0652392a2d7c9db2ae5b40210843c0bbc081cbd410825ab00cc59f14a6c
cmake_build libpng -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF

fetch freetype https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.xz \
	12991c4e55c506dd7f9b765933e62fd2be2e06d421505d7950a132e4f1bb484d
cmake_build freetype -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
	-DFT_DISABLE_PNG=ON -DFT_REQUIRE_ZLIB=ON

fetch glfw https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip \
	b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
cmake_build glfw -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF

#Shader translation: GLSL to SPIR-V, then SPIR-V to MSL with reflection. The command line
#tools (glslang, spirv-cross) are kept for checking shaders by hand.
fetch glslang https://github.com/KhronosGroup/glslang/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz \
	81038794e20494556edbcc0fc70fa984d71d1b440f9c49adf2cbaaa60a519757
cmake_build glslang -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DGLSLANG_TESTS=OFF -DBUILD_EXTERNAL=OFF \
	-DENABLE_GLSLANG_BINARIES=ON -DGLSLANG_ENABLE_INSTALL=ON

fetch spirv-cross https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz \
	97c910326afdd44d794ce8561326fa675fd1958b27142f03295403044d639639
#The CLI needs every backend, so they stay on
cmake_build spirv-cross -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=ON \
	-DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF

#Only static archives may be found in the prefix, so the linker can never pick a dylib
find "$PREFIX/lib" -name "*.dylib" -delete
echo "Dependencies installed to $PREFIX (macOS $MACOSX_DEPLOYMENT_TARGET)"
