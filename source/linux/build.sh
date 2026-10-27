#!/bin/bash
#Builds everything needed to get binaries

build_angelscript() {
	cd source/angelscript/projects/gnuc
	make clean
	if [[ "$(uname)" == "Darwin" ]]; then
		#The AngelScript makefile adds no optimisation flags, and on arm64 Macs all
		#scripts run in its interpreter since the game's JIT is x86 only
		CXXFLAGS="$ARCHFLAGS -O2" make all -j6
	else
		CXXFLAGS="$ARCHFLAGS" make all -j6
	fi
	cd ../../../../
	cp source/angelscript/lib/libangelscript.a $ODIR/libangelscript.a
}

build_breakpad() {
	cd source/breakpad
	if [[ "$ARCH" = "32" ]]; then
		./configure --build=x86
	else
		./configure --build=x86_64
	fi;
	make clean
	make -j6
	cp src/client/linux/libbreakpad_client.a ../../$ODIR/libbreakpad_client.a
	make clean
	cd ../../
}

args=$@
if [[ "$args" == "" ]]; then
	args="32 libs 64 libs"
fi
if [[ "$(uname)" != "Darwin" ]]; then
    export AR=gcc-ar
    export RANLIB=gcc-ranlib
    export CC=gcc
    export CXX=g++
    export OSNAME=lin
else
    export AR=ar
    export RANLIB=ranlib
    export OSNAME=osx
fi

for arg in $args; do
	case $arg in
		32)
			export ARCHFLAGS="-m32 -march=pentium4 -mtune=generic"
			export ARCH=32
		;;
		64)
			export ARCH=64
		;;
		debug)
			export ARCHFLAGS="$ARCHFLAGS -O0 -g"
            export ODIR="${ODIR:-obj_d/$OSNAME$ARCH}"
			mkdir -p "$ODIR"
		;;
		release)
			if [[ "$(uname)" == "Darwin" ]]; then
				export ARCHFLAGS="$ARCHFLAGS -O3"
			else
				export ARCHFLAGS="$ARCHFLAGS -Ofast"
			fi
			if [[ -z "$NLTO" ]]; then
				export ARCHFLAGS="$ARCHFLAGS -flto"
			else
				export ARCHFLAGS="$ARCHFLAGS -fno-lto"
			fi
			export ODIR="${ODIR:-obj/$OSNAME$ARCH}"
			mkdir -p "$ODIR"
		;;
		libs)
			build_angelscript
			build_breakpad
		;;
		angelscript)
			build_angelscript
		;;
		breakpad)
			build_breakpad
		;;
		version)
			rev=$(($(git rev-list HEAD --count) - 1150))
			sym="r"
			if [[ $(cat current_branch) == "stable" ]]; then
				sym="s"
			fi;

			v=$(head -n -1 source/game/main/version.h);
			echo -e "$v\n#define BUILD_VERSION \"$sym$rev\"\r" > source/game/main/version.h

			v=$(head -n -1 scripts/definitions/version.as);
			echo -e "$v\nconst string SCRIPT_VERSION = \"$sym$rev\";\r" > scripts/definitions/version.as
		;;
	esac
done

# vim: ff=unix:
