#!/bin/bash
# USBDiskKeeper.kext, built with the Command Line Tools only: MacKernelSDK 3f75008 and the Lilu SDK of Lilu-1.7.2-DEBUG.zip.
#   bash build.sh [Release|Debug] [outdir]      -> <outdir>/USBDiskKeeper.kext   (default: Release, build/Release)
#   MKSDK=/path/MacKernelSDK LILU_ZIP=/path/Lilu-1.7.2-DEBUG.zip bash build.sh ...   reuses local copies of the two SDKs;
#   without them they are fetched into build/deps.
# Release is optimised and stripped; a copy of its executable with the local symbols is kept beside the kext, to name
# a function from an address. Debug is -O0, keeps its symbols and has DBGLOG compiled in (-udkdbg turns it on).
# Reproducible: no debug info, and SOURCE_DATE_EPOCH, taken from the commit when the tree is a Git checkout, fixes the
# build date that Lilu's headers put in the version string.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CONF=${1:-Release}
case $CONF in Release|Debug) ;; *) echo "usage: bash build.sh [Release|Debug] [outdir]" >&2; exit 1 ;; esac
O=${2:-$HERE/build/$CONF}
mkdir -p "$O"; O=$(cd "$O" && pwd); B=$O/obj; D=$HERE/build/deps
NAME=USBDiskKeeper; ID=org.anv1552.$NAME
VER=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$HERE/Info.plist")
MKSDK_COMMIT=3f750085caa17ec3a7880f11c11bf4f48cd6a164
LILU_URL=https://github.com/acidanthera/Lilu/releases/download/1.7.2/Lilu-1.7.2-DEBUG.zip
LILU_SHA=e95df95d82e8b151047abf359ecd9c853755c59293e19dfcc3907da8fbd8cf89
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$HERE" log -1 --format=%ct 2>/dev/null || echo 1790000000)}

M=${MKSDK:-$D/MacKernelSDK}
[ -d "$M" ] || { mkdir -p "$D"; git clone -q https://github.com/acidanthera/MacKernelSDK.git "$M"; }
git -C "$M" cat-file -e "$MKSDK_COMMIT^{commit}" || { echo "STOP: $M lacks MacKernelSDK $MKSDK_COMMIT" >&2; exit 1; }
Z=${LILU_ZIP:-$D/Lilu-1.7.2-DEBUG.zip}
[ -f "$Z" ] || { mkdir -p "$D"; curl -fsSL -o "$Z" "$LILU_URL"; }
[ "$(shasum -a 256 "$Z" | cut -d' ' -f1)" = $LILU_SHA ] || { echo "STOP: $Z is not Lilu-1.7.2-DEBUG.zip ($LILU_SHA)" >&2; exit 1; }

# Only what this script made is replaced: the kext, the objects and the two unpacked SDKs.
for d in "$B" "$O/sdk" "$O/$NAME.kext"; do [ ! -e "$d" ] || rm -r "$d"; done
mkdir -p "$B" "$O/sdk/MacKernelSDK" "$O/$NAME.kext/Contents/MacOS"
git -C "$M" archive $MKSDK_COMMIT | tar -x -C "$O/sdk/MacKernelSDK"
unzip -q "$Z" -d "$O/sdk/lilu"
SDK=$O/sdk/lilu/Lilu.kext/Contents/Resources; K=$O/sdk/MacKernelSDK/Headers
[ -f "$SDK/Headers/plugin_start.hpp" ] || { echo "STOP: no Lilu SDK in $Z" >&2; exit 1; }

if [ "$CONF" = Debug ]; then OPT=(-O0 -DDEBUG); else OPT=(-O2 -flto); fi
FLAGS=(-arch x86_64 -mmacosx-version-min=10.6 -std=c++14 -mkernel -fapple-kext -nostdinc -I"$K" -I"$SDK" -I"$HERE"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DMODULE_VERSION=$VER -DPRODUCT_NAME=$NAME
  "${OPT[@]}" -fvisibility=hidden -fno-rtti -fno-exceptions -fno-builtin -fno-common -Wall -Wno-unused-parameter)
: > "$B/compile.log"
for f in "$SDK/Library/plugin_start.cpp" "$HERE/$NAME.cpp"; do
  xcrun clang++ "${FLAGS[@]}" -c "$f" -o "$B/$(basename "${f%.cpp}").o" 2>> "$B/compile.log" || { tail -40 "$B/compile.log" >&2; exit 1; }
done
cat > "$B/${NAME}_info.c" <<EOC
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__private_extern__ kern_return_t ${NAME}_kern_start(kmod_info_t *ki, void *data);
__private_extern__ kern_return_t ${NAME}_kern_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($ID, "$VER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = ${NAME}_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = ${NAME}_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
EOC
xcrun clang -arch x86_64 -mmacosx-version-min=10.6 -mkernel -nostdinc -I"$K" -DKERNEL -DKERNEL_PRIVATE "${OPT[@]}" \
  -fvisibility=hidden -c "$B/${NAME}_info.c" -o "$B/${NAME}_info.o"
BIN=$O/$NAME.kext/Contents/MacOS/$NAME
LTO=(); [ "$CONF" = Debug ] || LTO=(-flto)
xcrun clang++ -arch x86_64 -mmacosx-version-min=10.6 -mkernel -fapple-kext -nostdlib ${LTO[@]+"${LTO[@]}"} -Xlinker -kext -Xlinker -dead_strip \
  -static -L"$O/sdk/MacKernelSDK/Library/universal" "$B"/{plugin_start,$NAME,${NAME}_info}.o -lkmodc++ -lkmod -lcc_kext -o "$BIN"
if [ "$CONF" = Release ]; then
  cp "$BIN" "$O/$NAME.symbols"
  strip -S "$BIN"; strip -x -T "$BIN" 2>/dev/null || strip -x "$BIN"
fi
cp "$HERE/Info.plist" "$O/$NAME.kext/Contents/Info.plist"
echo "compile warnings: $(grep -c 'warning:' "$B/compile.log" || true)"
echo "$NAME $CONF $(shasum -a 256 "$BIN" | cut -c1-16) $(stat -f %z "$BIN") B, $(dwarfdump --uuid "$BIN" | awk '{print $2}') -> $O/$NAME.kext"
