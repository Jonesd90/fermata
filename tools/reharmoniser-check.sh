#!/bin/sh
# Builds the JUCE-free Re-HarmoniSer core with g++ and compares it with the Python reference results.
# Usage: tools/reharmoniser-check.sh <golden folder>   (make it with: python3 tools/reharmoniser/make_golden.py <folder>)
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -O2 -Wall -Wextra -pthread tests/reharmoniser_test.cpp src/reharmoniser/Engine.cpp src/reharmoniser/Dsp.cpp -o /tmp/reharmoniser_test
/tmp/reharmoniser_test "$1"
