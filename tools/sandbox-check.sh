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
$CXX -c "$HERE/src/core/DenoiseFft.cpp" -o "$OUT/DenoiseFft.o"
$CXX -c "$HERE/src/core/ImportPlan.cpp" -o "$OUT/ImportPlan.o"
$CXX -c "$HERE/src/core/Aaf.cpp" -o "$OUT/Aaf.o"
$CXX -c "$HERE/src/core/AafWrite.cpp" -o "$OUT/AafWrite.o"
$CXX -c "$HERE/src/core/AafExport.cpp" -o "$OUT/AafExport.o"
$CXX -c "$HERE/src/core/TakeMatch.cpp" -o "$OUT/TakeMatch.o"
$CXX -c "$HERE/src/core/Ltc.cpp" -o "$OUT/Ltc.o"
$CXX -c "$HERE/src/core/MasterDef.cpp" -o "$OUT/MasterDef.o"
$CXX -c "$HERE/src/core/Ddp.cpp" -o "$OUT/Ddp.o"
$CXX -c "$HERE/src/core/MasterExport.cpp" -o "$OUT/MasterExport.o"
$CXX -c "$HERE/src/core/ReviewPage.cpp" -o "$OUT/ReviewPage.o"
$CXX -c "$HERE/src/core/MasterRender.cpp" -o "$OUT/MasterRender.o"
$CXX -c "$HERE/src/core/Ravenna.cpp" -o "$OUT/Ravenna.o"
$CXX -c "$HERE/src/core/CoreReservation.cpp" -o "$OUT/CoreReservation.o"
$CXX -c "$HERE/src/core/CrashReport.cpp" -o "$OUT/CrashReport.o"
$CXX -c "$HERE/src/core/Retime.cpp" -o "$OUT/Retime.o"
$CXX -c "$HERE/src/core/VideoExport.cpp" -o "$OUT/VideoExport.o"
$CXX -c "$HERE/tests/core_test.cpp" -o "$OUT/core_test.o"
$CXX "$OUT"/*.o -o "$OUT/core_test" -lpthread -ldl -lrt
"$OUT/core_test"
# the concurrency stress test: a pretend audio thread against a pretend user, for a few seconds (longer: stress_test 30)
$CXX -c "$HERE/tests/stress_test.cpp" -o "$OUT/stress_test.o.x"
$CXX "$OUT/stress_test.o.x" $(ls "$OUT"/*.o | grep -v core_test.o) -o "$OUT/stress_test" -lpthread -ldl -lrt
"$OUT/stress_test" 3
# a long session, simulated faster than real time (3.1.3): three quarters of an hour here, two hours in tools/tool-checks.sh longsession
$CXX -c "$HERE/tests/long_session_test.cpp" -o "$OUT/long_session_test.o.x"
$CXX "$OUT/long_session_test.o.x" $(ls "$OUT"/*.o | grep -v core_test.o) -o "$OUT/long_session_test" -lpthread -ldl -lrt
"$OUT/long_session_test" 0.75
# the time readout's formats and parsing (only needs juce_core and the LTC code)
$CXX -c "$HERE/tests/timeformat_test.cpp" -o "$OUT/timeformat_test.o.x"
$CXX "$OUT/timeformat_test.o.x" "$OUT/Ltc.o" "$OUT"/juce_*.o "$OUT/ct.o" -o "$OUT/timeformat_test" -lpthread -ldl -lrt
"$OUT/timeformat_test"
# the control port's connections: lines in and out, a controller that stops reading, many threads at once (only needs juce_core)
$CXX -c "$HERE/tests/remote_hub_test.cpp" -o "$OUT/remote_hub_test.o.x"
$CXX "$OUT/remote_hub_test.o.x" "$OUT/juce_core.o" "$OUT/ct.o" -o "$OUT/remote_hub_test" -lpthread -ldl -lrt
"$OUT/remote_hub_test"
