#!/bin/bash
# Developer helper: syntax-checks the UI sources against JUCE headers on Linux. Usage: tools/syntax-check-ui.sh /path/to/JUCE [files...]
JUCE=${1:?path to JUCE}; shift
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODS="juce_core juce_events juce_graphics juce_data_structures juce_gui_basics juce_gui_extra juce_audio_basics juce_audio_formats juce_audio_devices juce_audio_processors juce_audio_utils"
DEFS="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_PLUGINHOST_VST3=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0"
for m in $MODS; do DEFS="$DEFS -DJUCE_MODULE_AVAILABLE_$m=1"; done
FILES=${@:-$(ls "$HERE"/src/ui/*.cpp "$HERE"/src/Main.cpp 2>/dev/null)}
rc=0
for f in $FILES; do echo "== $f"; g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -I"$JUCE/modules" $DEFS "$f" 2>&1 | grep -v "^In file included\|juce/modules" | head -40; [ ${PIPESTATUS[0]} -ne 0 ] && rc=1; done
exit $rc
