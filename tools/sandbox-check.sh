#!/bin/bash
# Developer helper: compiles the engine against JUCE on Linux (no audio hardware needed) and runs the tests.
# Usage: tools/sandbox-check.sh /path/to/JUCE
set -e
JUCE=${1:?path to JUCE checkout}
OUT=${2:-/tmp/takedaw-build}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUT"
DEFS="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DJUCE_ALSA=0 -DJUCE_JACK=0 -DJUCE_USE_XRANDR=0 -DJUCE_USE_XINERAMA=0 -DJUCE_USE_XCURSOR=0 -DJUCE_USE_XSHM=0 -DJUCE_MODAL_LOOPS_PERMITTED=0 -DNDEBUG=1"
CXX="g++ -std=c++17 -O1 -I$JUCE/modules $DEFS -w"
for m in juce_core juce_events juce_audio_basics juce_audio_formats juce_audio_devices juce_data_structures; do
  [ -f "$OUT/$m.o" ] || { echo "building $m"; echo "#include <$m/$m.cpp>" > "$OUT/$m.cpp"; $CXX -c "$OUT/$m.cpp" -o "$OUT/$m.o" & }
done
wait
echo "#include <juce_core/juce_core_CompilationTime.cpp>" > "$OUT/ct.cpp"; $CXX -c "$OUT/ct.cpp" -o "$OUT/ct.o"
$CXX -c "$HERE/src/core/Project.cpp" -o "$OUT/Project.o"
$CXX -c "$HERE/src/core/Playback.cpp" -o "$OUT/Playback.o"
$CXX -c "$HERE/src/core/AudioEngine.cpp" -o "$OUT/AudioEngine.o"
$CXX -c "$HERE/src/core/Bounce.cpp" -o "$OUT/Bounce.o"
$CXX -c "$HERE/src/core/AudioOps.cpp" -o "$OUT/AudioOps.o"
$CXX -c "$HERE/src/core/ImportPlan.cpp" -o "$OUT/ImportPlan.o"
$CXX -c "$HERE/src/core/MasterDef.cpp" -o "$OUT/MasterDef.o"
$CXX -c "$HERE/src/core/Ddp.cpp" -o "$OUT/Ddp.o"
$CXX -c "$HERE/src/core/MasterExport.cpp" -o "$OUT/MasterExport.o"
$CXX -c "$HERE/src/core/MasterRender.cpp" -o "$OUT/MasterRender.o"
$CXX -c "$HERE/src/core/Ravenna.cpp" -o "$OUT/Ravenna.o"
$CXX -c "$HERE/tests/core_test.cpp" -o "$OUT/core_test.o"
$CXX "$OUT"/*.o -o "$OUT/core_test" -lpthread -ldl -lrt
"$OUT/core_test"
