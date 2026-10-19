#!/bin/bash
# Developer helper: compiles (syntax only) the Windows parts of src/core/CoreReservation.cpp and src/core/FileSync.h on Linux, against
# tools/win-stubs/windows.h. Usage: tools/check-windows-code.sh /path/to/JUCE
JUCE=${1:?path to JUCE}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
FLAGS="-std=c++17 -fsyntax-only -Wall -Wextra -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_MODULE_AVAILABLE_juce_core=1 -I$HERE/tools/win-stubs -isystem $JUCE/modules"
g++ $FLAGS -DFERMATA_CPU_WINDOWS_STUBS=1 "$HERE/src/core/CoreReservation.cpp" || exit 1
echo '#include "src/core/FileSync.h"
int main() { return td::filesync::syncToDisk (juce::File()) ? 0 : 1; }' | g++ $FLAGS -DFERMATA_FILESYNC_WINDOWS_STUBS=1 -I"$HERE" -x c++ - || exit 1
echo "Windows code: OK"
