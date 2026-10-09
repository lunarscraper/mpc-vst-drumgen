#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan): run after build.sh (needs build/params.h, build/popup.h).
# Runs host_test twice: with its own test templates (the NGEN factory files are not in this repo)
# and with no template folder at all. Prints PASSED/FAILED; exit code follows.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD/..":/b -w /b/vst gcc:12 bash -euc 'set -o pipefail
  mkdir -p build/x86
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -Wall -Wextra -Wno-unused-parameter -DNO_ALSA -fPIC \
      -Ibuild -I. -shared -o build/x86/drumgen-x86.so drumgen_vst.cpp -lpthread -ldl
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -o build/x86/host_test host_test.cpp -ldl
  ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/drumgen-x86.so fixture | grep -v "^MIDI"
  ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/drumgen-x86.so none | grep -v "^MIDI"
'
