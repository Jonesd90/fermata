#!/bin/bash
# Developer helper: compiles (syntax only) the Windows part of src/core/CoreReservation.cpp and src/core/CrashReport.cpp on Linux, against tools/win-stubs/windows.h, checks
# that src/core/FileSync.h compiles on its own (3.1.3: it no longer has a Windows branch - its disk sync goes through JUCE's stream), and scans
# every source for identifiers that windows.h turns into macros (small, near, far, hyper, pascal: MSVC then fails with "'int' followed by 'char'
# is illegal" or "type 'char' unexpected" in any file that includes a Windows header).
# Usage: tools/check-windows-code.sh /path/to/JUCE
JUCE=${1:?path to JUCE}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
FLAGS="-std=c++17 -fsyntax-only -Wall -Wextra -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_MODULE_AVAILABLE_juce_core=1 -I$HERE/tools/win-stubs -isystem $JUCE/modules"
g++ $FLAGS -DFERMATA_CPU_WINDOWS_STUBS=1 "$HERE/src/core/CoreReservation.cpp" || exit 1
g++ $FLAGS -DFERMATA_CPU_WINDOWS_STUBS=1 "$HERE/src/core/CrashReport.cpp" || exit 1
echo '#include "src/core/FileSync.h"
int main() { return td::filesync::syncToDisk (juce::File()) ? 0 : 1; }' | g++ $FLAGS -I"$HERE" -x c++ - || exit 1
python3 - "$HERE" <<'PY' || exit 1
import re, sys, glob, os
root = sys.argv[1]
names = r'\b(small|near|far|hyper|pascal)\b'
bad = []
for f in glob.glob(root + '/src/**/*.cpp', recursive=True) + glob.glob(root + '/src/**/*.h', recursive=True):
    if 'third_party' in f: continue
    code = open(f, encoding='utf-8', errors='replace').read()
    code = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), code, flags=re.S)
    code = re.sub(r'//[^\n]*', '', code)
    code = re.sub(r'"(\\.|[^"\\])*"', '""', code)
    for i, line in enumerate(code.split('\n'), 1):
        if line.lstrip().startswith('#'): continue
        if re.search(names, line): bad.append(f'{os.path.relpath(f, root)}:{i}: {line.strip()[:100]}')
if bad:
    print('identifiers that are Windows macro names (rename them):'); print('\n'.join(bad)); sys.exit(1)
PY
echo "Windows code: OK"
