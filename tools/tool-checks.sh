#!/bin/bash
# Developer helper (3.0.0): the checks where the tools, not the reasoning, find the problems. Each mode builds in its own folder under the out
# folder; the normal builds are not touched.
#
#   asan      AddressSanitizer + UndefinedBehaviorSanitizer (and the leak check): the core tests, the stress test, the time-format and
#             control-port tests, the Re-HarmoniSer's voices test (and its golden test with REHAR_GOLDEN=<folder from make_golden.py>)
#   tsan      ThreadSanitizer: the stress test, the control-port test and the core tests
#   fuzz      the readers' fuzz test (tests/fuzz_readers.cpp) under the address and undefined-behaviour checks, FUZZ_SECONDS per target
#             (default 60). FUZZ_AAF=<an .aaf file> adds the AAF reader; FUZZ_PROJECTS=<folder:folder> adds saved projects to start from.
#   warnings  every source compiled by GCC with the bug-finding warnings on (one log per file)
#   tidy      clang-tidy's bugprone and static-analyzer checks on every source (one log per file)
#   query     clang-query: every place a juce::var is indexed with a number (JUCE does not check that in a release build: a value that is
#             not a list, or a number outside it, crashes - so each place must have checked first)
#   valgrind  valgrind's memcheck on the stress test, the time-format and control-port tests: memory read before it was ever written
#   coverage  which lines of src/core the core tests and the stress test run (gcov), per file, least covered first
#   rtcheck   (3.1.2) the audio path never waits: the core tests and the stress test with tests/realtime_guard.cpp linked in, which reports every
#             allocation, lock, sleep, file or socket call made while inside AudioEngine::process, with the place it came from
#   longsession  (3.1.3) tests/long_session_test.cpp: LONG_HOURS simulated hours (default 2) of takes, saves, meters and playbacks, faster than real
#             time, watching for memory that grows, lists that never shrink and timing that drifts (about 3 minutes per simulated hour)
#   all       all of the above, one after another
#
# Usage: tools/tool-checks.sh /path/to/JUCE <mode> [out folder]          (out folder: default /tmp/fermata-tool-checks)
# Each mode ends with a summary line per program or a count per file; the logs are in the out folder.
# Two core test programs must never run at the same time (they share a temporary folder): run one mode at a time.
JUCE=${1:?path to JUCE}
MODE=${2:?mode: asan tsan fuzz warnings tidy query valgrind coverage rtcheck longsession all}
OUT=${3:-/tmp/fermata-tool-checks}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUT"
JOBS=$(nproc 2>/dev/null || echo 2)
CORE_DEFS="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DJUCE_ALSA=0 -DJUCE_JACK=0 -DJUCE_USE_XRANDR=0 -DJUCE_USE_XINERAMA=0 -DJUCE_USE_XCURSOR=0 -DJUCE_USE_XSHM=0 -DJUCE_MODAL_LOOPS_PERMITTED=0 -DNDEBUG=1"
GUI_MODS="juce_core juce_events juce_graphics juce_data_structures juce_gui_basics juce_gui_extra juce_audio_basics juce_audio_formats juce_audio_devices juce_audio_processors juce_audio_utils"
GUI_DEFS="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_PLUGINHOST_VST3=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DJUCE_ALSA=0 -DJUCE_JACK=0 -DNDEBUG=1 -DJUCE_DISPLAY_SPLASH_SCREEN=0 -DFERMATA_VERSION_STRING=\"tool-checks\""
for m in $GUI_MODS; do GUI_DEFS="$GUI_DEFS -DJUCE_MODULE_AVAILABLE_$m=1"; done
GUI_ARGS="-std=c++17 -isystem $JUCE/modules -I/usr/include/freetype2 -I$HERE/tools/win-stubs $GUI_DEFS"
ALL_SOURCES=$(ls "$HERE"/src/core/*.cpp "$HERE"/src/ui/*.cpp "$HERE"/src/reharmoniser/*.cpp "$HERE"/src/Main.cpp)
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0:suppressions=$HERE/tools/ubsan-suppressions.txt"
# the only leak the tests make on purpose: the plan an audio thread that did not come round may still be using is left in memory (see
# AudioEngine::rebuildPlan); the tests pretend such a stall
printf 'leak:td::AudioEngine::rebuildPlan\n' > "$OUT/lsan-suppressions.txt"

# build_core <folder> <compiler and flags> [compiler and flags for JUCE]: the JUCE modules the core needs, and every src/core file, into <folder>;
# CORE_OBJS lists the core's objects, JUCE_OBJS JUCE's
build_core ()
{
    local O="$1" CXX="$2" CXXJ="${3:-$2}"
    mkdir -p "$O"
    for m in juce_core juce_events juce_audio_basics juce_audio_formats juce_audio_devices juce_data_structures; do
        [ -f "$O/$m.o" ] || { echo "#include <$m/$m.cpp>" > "$O/$m.cpp"; echo "$O/$m.cpp"; }
    done | xargs -r -P"$JOBS" -I{} bash -c 'f={}; '"$CXXJ"' -c $f -o ${f%.cpp}.o || echo "FAILED $f"'
    [ -f "$O/ct.o" ] || { echo "#include <juce_core/juce_core_CompilationTime.cpp>" > "$O/ct.cpp"; $CXXJ -c "$O/ct.cpp" -o "$O/ct.o"; }
    ls "$HERE"/src/core/*.cpp | xargs -P"$JOBS" -I{} bash -c 'f={}; b=$(basename $f .cpp); '"$CXX"' -c $f -o '"$O"'/core_$b.o 2>'"$O"'/core_$b.log || echo "FAILED $b (see core_$b.log)"'
    CORE_OBJS=""; for f in "$HERE"/src/core/*.cpp; do CORE_OBJS="$CORE_OBJS $O/core_$(basename "$f" .cpp).o"; done      # (named, not globbed: no stale object is linked)
    JUCE_OBJS="$O/juce_core.o $O/juce_events.o $O/juce_audio_basics.o $O/juce_audio_formats.o $O/juce_audio_devices.o $O/juce_data_structures.o $O/ct.o"
}

# report <log> <pattern> <what>: one summary line for a test program's run
report ()
{
    printf '%-18s exit %-3s %3s %s; %s\n' "$(basename "$1" .log)" "$2" "$(grep -c -E "$3" "$1")" "$4" "$(grep -E 'PASSED|FAILURE' "$1" | tail -1)"
}

run_asan ()
{
    local O="$OUT/asan" CXX="g++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -I$JUCE/modules $CORE_DEFS -w"
    echo "== address and undefined-behaviour checks ($O)"
    build_core "$O" "$CXX"
    local L="-fsanitize=address,undefined -lpthread -ldl -lrt"
    for t in core_test stress_test timeformat_test remote_hub_test; do $CXX -c "$HERE/tests/$t.cpp" -o "$O/t_$t.o" 2>"$O/t_$t.log" || echo "FAILED $t"; done
    g++ "$O/t_core_test.o" $CORE_OBJS $JUCE_OBJS -o "$O/core_test" $L
    g++ "$O/t_stress_test.o" $CORE_OBJS $JUCE_OBJS -o "$O/stress_test" $L
    g++ "$O/t_timeformat_test.o" "$O/core_Ltc.o" $JUCE_OBJS -o "$O/timeformat_test" $L
    g++ "$O/t_remote_hub_test.o" "$O/juce_core.o" "$O/ct.o" -o "$O/remote_hub_test" $L
    # the Re-HarmoniSer's engine (no JUCE): its voices test, and its golden test when REHAR_GOLDEN names a folder made by tools/reharmoniser/make_golden.py
    local RH="g++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -pthread"
    $RH "$HERE/tests/reharmoniser_voices_test.cpp" "$HERE/src/reharmoniser/Engine.cpp" "$HERE/src/reharmoniser/Dsp.cpp" -o "$O/reharmoniser_voices_test" 2>"$O/t_rehar_voices.log" || echo "FAILED reharmoniser_voices_test"
    [ -n "$REHAR_GOLDEN" ] && { $RH "$HERE/tests/reharmoniser_test.cpp" "$HERE/src/reharmoniser/Engine.cpp" "$HERE/src/reharmoniser/Spectrogram.cpp" "$HERE/src/reharmoniser/Dsp.cpp" -o "$O/reharmoniser_test" 2>"$O/t_rehar.log" || echo "FAILED reharmoniser_test"; }
    export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:print_summary=1" LSAN_OPTIONS="suppressions=$OUT/lsan-suppressions.txt"
    for t in core_test stress_test timeformat_test remote_hub_test reharmoniser_voices_test reharmoniser_test; do
        [ -x "$O/$t" ] || continue
        [ "$t" = reharmoniser_test ] && [ -z "$REHAR_GOLDEN" ] && continue
        ( cd "$O" && if [ "$t" = stress_test ]; then ./$t 3; elif [ "$t" = reharmoniser_test ]; then ./$t "$REHAR_GOLDEN"; else ./$t; fi ) > "$OUT/asan_$t.log" 2>&1
        report "$OUT/asan_$t.log" $? "ERROR: AddressSanitizer|runtime error:|ERROR: LeakSanitizer" "memory / undefined-behaviour / leak report(s)"
    done
}

run_tsan ()
{
    local O="$OUT/tsan" CXX="g++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=thread -I$JUCE/modules $CORE_DEFS -w"
    echo "== thread checks ($O)"
    build_core "$O" "$CXX"
    local L="-fsanitize=thread -lpthread -ldl -lrt"
    for t in core_test stress_test remote_hub_test; do $CXX -c "$HERE/tests/$t.cpp" -o "$O/t_$t.o" 2>"$O/t_$t.log" || echo "FAILED $t"; done
    g++ "$O/t_core_test.o" $CORE_OBJS $JUCE_OBJS -o "$O/core_test" $L
    g++ "$O/t_stress_test.o" $CORE_OBJS $JUCE_OBJS -o "$O/stress_test" $L
    g++ "$O/t_remote_hub_test.o" "$O/juce_core.o" "$O/ct.o" -o "$O/remote_hub_test" $L
    export TSAN_OPTIONS="halt_on_error=0:report_signal_unsafe=0:second_deadlock_stack=1"
    for t in stress_test remote_hub_test core_test; do
        ( cd "$O" && if [ "$t" = stress_test ]; then ./$t 5; else ./$t; fi ) > "$OUT/tsan_$t.log" 2>&1
        report "$OUT/tsan_$t.log" $? "WARNING: ThreadSanitizer" "thread report(s)"
    done
}

run_fuzz ()
{
    local O="$OUT/asan" CXX="g++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -I$JUCE/modules $CORE_DEFS -w"
    echo "== the readers' fuzz test ($O, ${FUZZ_SECONDS:-60} s per target)"
    build_core "$O" "$CXX"
    $CXX -c "$HERE/tests/fuzz_readers.cpp" -o "$O/t_fuzz_readers.o" 2>"$O/t_fuzz_readers.log" || { echo "FAILED fuzz_readers (see t_fuzz_readers.log)"; return; }
    g++ "$O/t_fuzz_readers.o" $CORE_OBJS $JUCE_OBJS -o "$O/fuzz_readers" -fsanitize=address,undefined -lpthread -ldl -lrt
    mkdir -p "$OUT/fuzz"
    export ASAN_OPTIONS="detect_leaks=0:handle_abort=1:allocator_may_return_null=0:max_allocation_size_mb=2048"
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:suppressions=$HERE/tools/ubsan-suppressions.txt"
    ( cd "$OUT/fuzz" && "$O/fuzz_readers" "${FUZZ_SECONDS:-60}" ) > "$OUT/fuzz.log" 2>&1
    local rc=$?
    # stopped by undefined behaviour (UBSan's exit runs no callback): the input it was trying is still in the work folder
    local W=$(sed -n 's/^work folder: //p' "$OUT/fuzz.log" | head -1)
    if [ $rc -ne 0 ] && [ -n "$W" ] && [ -f "$W/last-input.bin" ]; then
        cp "$W/last-input.bin" "$OUT/fuzz/last-input-$(cat "$W/last-target.txt" 2>/dev/null).bin"; rm -rf "$W"
    fi
    echo "fuzz_readers exit $rc  (inputs kept in $OUT/fuzz if anything was found: fuzz_readers --replay <target> <file>)"
    grep -E "inputs,|FUZZ:|FINDING|HANG|ERROR: AddressSanitizer|runtime error" "$OUT/fuzz.log" | head -40
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0:suppressions=$HERE/tools/ubsan-suppressions.txt"
}

run_warnings ()
{
    local O="$OUT/warnings"; mkdir -p "$O"
    echo "== GCC with the bug-finding warnings ($O)"
    local W="-Wall -Wextra -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wimplicit-fallthrough -Wmisleading-indentation -Wformat=2 -Wpessimizing-move -Wredundant-move -Wrange-loop-construct -Wdangling-reference -Wuninitialized -Wmaybe-uninitialized -Wno-unused-parameter"
    export CXXW="g++ -O2 $W $GUI_ARGS" O
    echo "$ALL_SOURCES" | xargs -P"$JOBS" -I{} bash -c 'f={}; b=$(basename $f .cpp); $CXXW -c $f -o /dev/null 2>$O/$b.log; echo "$b $(grep -c "warning:" $O/$b.log) warning(s) $(grep -c "error:" $O/$b.log) error(s)"' | sort
    echo "by kind:"; cat "$O"/*.log | grep "warning:" | grep -v "/third_party/" | sed -E 's/.*\[(-W[a-z0-9=-]+)\]$/\1/' | sort | uniq -c | sort -rn
}

run_tidy ()
{
    local O="$OUT/tidy"; mkdir -p "$O"
    echo "== clang-tidy, bug-prone and static-analyzer checks ($O)"
    export CHECKS='-*,bugprone-*,clang-analyzer-*,-bugprone-easily-swappable-parameters,-bugprone-narrowing-conversions,-bugprone-implicit-widening-of-multiplication-result,-bugprone-branch-clone,-bugprone-reserved-identifier,-bugprone-macro-parentheses,-bugprone-integer-division,-bugprone-switch-missing-default-case,-bugprone-unchecked-optional-access,-bugprone-exception-escape,-clang-analyzer-optin.cplusplus.VirtualCall,-bugprone-misplaced-widening-cast,-bugprone-chained-comparison'
    export ARGS="$GUI_ARGS" O
    echo "$ALL_SOURCES" | xargs -P"$JOBS" -I{} bash -c 'f={}; b=$(basename $f .cpp); clang-tidy -checks="$CHECKS" -header-filter=".*/src/.*" "$f" -- $ARGS > $O/$b.log 2>&1; echo "$b $(grep -c "warning:" $O/$b.log) finding(s)"' | sort
    echo "by check:"; cat "$O"/*.log | grep -E "warning:.*\[" | grep -v "/third_party/" | sed -E 's/.*\[([a-z0-9.,-]+)\]$/\1/' | sort | uniq -c | sort -rn
}

run_query ()
{
    local O="$OUT/query"; mkdir -p "$O"
    local QUERY=$(command -v clang-query || command -v clang-query-18)
    [ -n "$QUERY" ] || { echo "query: clang-query is not installed"; return; }
    echo "== clang-query: juce::var indexed with a number ($O)"
    export QUERY ARGS="$GUI_ARGS" O
    export Q='match cxxOperatorCallExpr(callee(cxxMethodDecl(hasName("operator[]"), ofClass(hasName("::juce::var")), parameterCountIs(1), hasParameter(0, hasType(isInteger())))))'
    echo "$ALL_SOURCES" | xargs -P"$JOBS" -I{} bash -c 'f={}; b=$(basename $f .cpp); $QUERY -c "set output diag" -c "$Q" "$f" -- $ARGS > $O/$b.log 2>&1'
    cat "$O"/*.log | grep -E '/src/.*"root" binds here' | sed -E "s#^$HERE/##; s/: note: \"root\" binds here//" | sort -u
}

run_valgrind ()
{
    command -v valgrind > /dev/null || { echo "valgrind: not installed"; return; }
    local O="$OUT/plain" CXX="g++ -std=c++17 -O1 -g -I$JUCE/modules $CORE_DEFS -w"
    echo "== valgrind: memory read before it was ever written (what the address checks do not see) ($O)"
    build_core "$O" "$CXX"
    local L="-lpthread -ldl -lrt"
    for t in stress_test timeformat_test remote_hub_test; do $CXX -c "$HERE/tests/$t.cpp" -o "$O/t_$t.o" 2>"$O/t_$t.log" || echo "FAILED $t"; done
    g++ "$O/t_stress_test.o" $CORE_OBJS $JUCE_OBJS -o "$O/stress_test" $L
    g++ "$O/t_timeformat_test.o" "$O/core_Ltc.o" $JUCE_OBJS -o "$O/timeformat_test" $L
    g++ "$O/t_remote_hub_test.o" "$O/juce_core.o" "$O/ct.o" -o "$O/remote_hub_test" $L
    for t in stress_test timeformat_test remote_hub_test; do
        ( cd "$O" && if [ "$t" = stress_test ]; then valgrind --track-origins=yes --error-exitcode=9 ./$t 5; else valgrind --track-origins=yes --error-exitcode=9 ./$t; fi ) > "$OUT/valgrind_$t.log" 2>&1
        echo "$t exit $?: $(grep 'ERROR SUMMARY' "$OUT/valgrind_$t.log" | tail -1 | sed 's/^==[0-9]*== //'); $(grep -E 'PASSED|FAILURE' "$OUT/valgrind_$t.log" | tail -1)"
    done
}

run_coverage ()
{
    local O="$OUT/coverage"
    echo "== test coverage: the lines of src/core that the core tests and the stress test run ($O)"
    build_core "$O" "g++ -std=c++17 -O1 -g --coverage -I$JUCE/modules $CORE_DEFS -w" "g++ -std=c++17 -O1 -I$JUCE/modules $CORE_DEFS -w"
    find "$O" -name '*.gcda' -delete
    local CXX="g++ -std=c++17 -O1 --coverage -I$JUCE/modules $CORE_DEFS -w"        # (the tests too: header-only code is counted where the tests use it)
    for t in core_test stress_test; do $CXX -c "$HERE/tests/$t.cpp" -o "$O/t_$t.o" 2>"$O/t_$t.log" || echo "FAILED $t"; done
    g++ --coverage "$O/t_core_test.o" $CORE_OBJS $JUCE_OBJS -o "$O/core_test" -lpthread -ldl -lrt
    g++ --coverage "$O/t_stress_test.o" $CORE_OBJS $JUCE_OBJS -o "$O/stress_test" -lpthread -ldl -lrt
    ( cd "$O" && ./core_test > "$OUT/coverage_core_test.log" 2>&1; ./stress_test 3 > "$OUT/coverage_stress_test.log" 2>&1 )
    ( cd "$O" && for f in "$HERE"/src/core/*.cpp; do gcov -n -o "$O/core_$(basename "$f" .cpp).o" "$f" 2>/dev/null; done
                 for t in core_test stress_test; do gcov -n -o "$O/t_$t.o" "$HERE/tests/$t.cpp" 2>/dev/null; done ) > "$OUT/coverage_gcov.txt"
    # one line per file of src/core (a header used by several programs counts where it was run most)
    python3 - "$OUT/coverage_gcov.txt" <<'PY'
import re, sys
best = {}
cur = None
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r"File '(.*)'", line)
    if m: cur = m.group(1); continue
    m = re.match(r"Lines executed:([0-9.]+)% of ([0-9]+)", line)
    if m and cur and '/src/core/' in cur and '/third_party/' not in cur:
        pct, n = float(m.group(1)), int(m.group(2)); name = cur.split('/src/core/')[1]
        if name not in best or pct > best[name][0]: best[name] = (pct, n)
        cur = None
tot = sum(n for p, n in best.values()); run = sum(p * n / 100 for p, n in best.values())
for name, (p, n) in sorted(best.items(), key=lambda kv: kv[1][0]): print(f"{p:6.1f}% of {n:5d} lines  {name}")
print(f"all of src/core: {100 * run / max(1, tot):.1f}% of {tot} lines run by the tests")
PY
}

run_rtcheck ()
{
    local O="$OUT/plain" CXX="g++ -std=c++17 -O1 -g -I$JUCE/modules $CORE_DEFS -w"
    echo "== realtime check: nothing inside AudioEngine::process may allocate, lock, sleep or touch a file or socket ($O)"
    build_core "$O" "$CXX"
    g++ -std=c++17 -O1 -g -c "$HERE/tests/realtime_guard.cpp" -o "$O/realtime_guard.o" 2>"$O/t_realtime_guard.log" || { echo "FAILED realtime_guard (see t_realtime_guard.log)"; return; }
    # the guard wraps AudioEngine::process (so it knows when a thread is on the audio path) and the two JUCE calls that wake another thread (allowed)
    local L="-rdynamic -lpthread -ldl -lrt -Wl,--wrap=_ZN2td11AudioEngine7processEPKPKfiPKPfii -Wl,--wrap=_ZNK4juce13WaitableEvent6signalEv -Wl,--wrap=_ZNK4juce6Thread6notifyEv"
    # first the guard's own check: a pretend engine that allocates, frees, locks and sleeps must give four kinds of report (else a clean run proves nothing)
    $CXX -DRTGUARD_CONTROL_ENGINE -c "$HERE/tests/realtime_guard_control.cpp" -o "$O/rt_control_engine.o" 2>"$O/t_rt_control.log" || echo "FAILED realtime_guard_control (engine)"
    $CXX -DRTGUARD_CONTROL_MAIN -c "$HERE/tests/realtime_guard_control.cpp" -o "$O/rt_control_main.o" 2>>"$O/t_rt_control.log" || echo "FAILED realtime_guard_control (main)"
    g++ "$O/rt_control_main.o" "$O/rt_control_engine.o" "$O/realtime_guard.o" "$O/juce_core.o" "$O/juce_events.o" "$O/ct.o" -o "$O/rt_control" $L
    ( cd "$O" && ./rt_control ) > "$OUT/rtcheck_control.log" 2>&1
    local ok=1; for k in malloc free pthread_mutex_lock nanosleep; do grep -q "^REALTIME: $k " "$OUT/rtcheck_control.log" || ok=0; done
    [ $ok = 1 ] && echo "guard self-check: OK (an allocation, a free, a lock and a sleep inside the audio callback were all reported)" || { echo "guard self-check FAILED (see rtcheck_control.log): the results below mean nothing"; }
    for t in core_test stress_test; do $CXX -c "$HERE/tests/$t.cpp" -o "$O/t_$t.o" 2>"$O/t_$t.log" || echo "FAILED $t"; done
    g++ "$O/t_core_test.o" "$O/realtime_guard.o" $CORE_OBJS $JUCE_OBJS -o "$O/rt_core_test" $L
    g++ "$O/t_stress_test.o" "$O/realtime_guard.o" $CORE_OBJS $JUCE_OBJS -o "$O/rt_stress_test" $L
    for t in core_test stress_test; do
        ( cd "$O" && if [ "$t" = stress_test ]; then ./rt_$t 5; else ./rt_$t; fi ) > "$OUT/rtcheck_$t.log" 2>&1
        report "$OUT/rtcheck_$t.log" $? "^REALTIME: " "realtime report(s)"
        # each place once, as source lines (addr2line on the offsets backtrace_symbols_fd printed)
        python3 - "$OUT/rtcheck_$t.log" "$O/rt_$t" <<'PY'
import re, subprocess, sys
log, exe = sys.argv[1], sys.argv[2]
blocks, cur = [], None
for line in open(log, errors='replace'):
    if line.startswith('REALTIME: '): cur = [line.strip(), []]; blocks.append(cur); continue
    if cur is None: continue
    if line.startswith('REALTIME-END'): cur = None; continue
    m = re.search(r'\(\+(0x[0-9a-f]+)\)', line)
    if m: cur[1].append(m.group(1))
for head, offs in blocks:
    print('  ' + head)
    if not offs: continue
    out = subprocess.run(['addr2line', '-e', exe, '-f', '-C', '-i', '-p'] + offs, capture_output=True, text=True).stdout.splitlines()
    shown = 0
    for l in out:
        if '/src/' in l or '/tests/' in l:
            print('      ' + re.sub(r' at .*/(src|tests)/', ' at \\1/', l.strip())); shown += 1
            if shown >= 8: break
PY
    done
}

run_longsession ()
{
    local O="$OUT/plain" CXX="g++ -std=c++17 -O1 -g -I$JUCE/modules $CORE_DEFS -w"
    echo "== a long session, ${LONG_HOURS:-2} simulated hours ($O)"
    build_core "$O" "$CXX"
    $CXX -c "$HERE/tests/long_session_test.cpp" -o "$O/t_long_session_test.o" 2>"$O/t_long_session_test.log" || { echo "FAILED long_session_test (see t_long_session_test.log)"; return; }
    g++ "$O/t_long_session_test.o" $CORE_OBJS $JUCE_OBJS -o "$O/long_session_test" -lpthread -ldl -lrt
    ( cd "$O" && ./long_session_test "${LONG_HOURS:-2}" ) > "$OUT/longsession.log" 2>&1
    local rc=$?
    grep -E "^long session|^  heap|^  lists|FAIL|PASSED|FAILED" "$OUT/longsession.log"
    echo "long_session_test exit $rc"
}

case "$MODE" in
    asan) run_asan ;; tsan) run_tsan ;; fuzz) run_fuzz ;; warnings) run_warnings ;; tidy) run_tidy ;; query) run_query ;;
    valgrind) run_valgrind ;; coverage) run_coverage ;; rtcheck) run_rtcheck ;; longsession) run_longsession ;;
    all) run_asan; run_tsan; run_fuzz; run_warnings; run_tidy; run_query; run_valgrind; run_coverage; run_rtcheck; run_longsession ;;
    *) echo "unknown mode: $MODE"; exit 2 ;;
esac
