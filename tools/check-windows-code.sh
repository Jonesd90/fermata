#!/bin/bash
# Developer helper: compiles (syntax only) the Windows part of src/core/CoreReservation.cpp on Linux, against tools/win-stubs/windows.h. Usage: tools/check-windows-code.sh /path/to/JUCE
JUCE=${1:?path to JUCE}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
g++ -std=c++17 -fsyntax-only -Wall -Wextra -DFERMATA_CPU_WINDOWS_STUBS=1 -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_MODULE_AVAILABLE_juce_core=1 \
    -I"$HERE/tools/win-stubs" -isystem "$JUCE/modules" "$HERE/src/core/CoreReservation.cpp"
