#!/bin/sh
# Builds the harness into the sandbox:
#   glut32.dll      the permanent loader (loader.c); the game's own glut32.dll is kept as glut32_real.dll.
#                   It is locked while the game runs: when it changed, stop the game first.
#   wumrt_core.dll  everything else (wumrt.c, rt_core.c, bvh.c). The loader runs a copy of it, so it can be
#                   rebuilt while the game runs; `python rt.py reload-core` swaps the new one in.
# Also copies the compute shaders and the ini files.
set -e
cd "$(dirname "$0")"
SB=../../rt-sandbox
CL="${CLANG:-/g/Program Files/LLVM/bin/clang.exe}"
MS="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
WK="C:/Program Files (x86)/Windows Kits/10"; V=10.0.26100.0
[ -f "$SB/glut32_real.dll" ] || cp "$SB/glut32.dll" "$SB/glut32_real.dll"
mkdir -p build
FLAGS="--target=i686-pc-windows-msvc -O1 -Wall -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-deprecated-declarations"
link() {
  "$CL" $FLAGS -shared "$@" \
    -idirafter "$MS/include" -idirafter "$WK/Include/$V/ucrt" -idirafter "$WK/Include/$V/um" -idirafter "$WK/Include/$V/shared" \
    -fuse-ld=lld -Xlinker "/libpath:$MS/lib/x86" -Xlinker "/libpath:$WK/Lib/$V/ucrt/x86" -Xlinker "/libpath:$WK/Lib/$V/um/x86" \
    -Xlinker -Brepro -lopengl32 -lgdi32 -luser32
}
link loader.c -o build/glut32.dll -Xlinker /def:wumrt.def
link -DWUMRT_CORE wumrt.c rt_core.c bvh.c fx.c post.c atmos.c -o build/wumrt_core.dll
cp build/wumrt_core.dll "$SB/wumrt_core.dll"
if cmp -s build/glut32.dll "$SB/glut32.dll"; then
  :
elif cp build/glut32.dll "$SB/glut32.dll" 2>/dev/null; then
  echo "loader updated"
else
  echo "loader changed but glut32.dll is in use: stop the game (python sandbox.py stop) and build again"
fi
cp wumrt_trace.comp wumrt_filter.comp wumrt_temporal.comp wumrt_atrous.comp wumrt_fx.comp wumrt_post.comp wumrt_atmos.comp wumrt_materials.ini wumrt_surfaces.ini wumrt_characters.ini wumrt_grades.ini "$SB/"
echo built
