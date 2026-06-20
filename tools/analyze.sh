#!/usr/bin/env bash
#
# tools/analyze.sh — one-command analysis gate for Oreshnek.
#
# Always runs the dynamic analyzers that ship with the compiler (Address +
# UndefinedBehavior, then Thread sanitizer) over the full test suite. Optional
# static/dynamic tools (clang-tidy, cppcheck, valgrind) are run when present and
# skipped with an install hint otherwise — so the gate is useful on any machine.
#
# Usage:
#   tools/analyze.sh            # full gate
#   tools/analyze.sh --no-tsan  # skip ThreadSanitizer (faster)
#
# Exit code is non-zero if any sanitizer build/test fails.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_TSAN=1
[ "${1:-}" = "--no-tsan" ] && RUN_TSAN=0

fail=0
section() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
have()    { command -v "$1" >/dev/null 2>&1; }

# --- AddressSanitizer + UBSan -------------------------------------------------
section "AddressSanitizer + UndefinedBehaviorSanitizer"
if cmake -S . -B build-asan -DORESHNEK_ASAN=ON -DORESHNEK_BUILD_EXAMPLES=OFF >/dev/null 2>&1 \
   && cmake --build build-asan -j >/dev/null 2>&1; then
    # LeakSanitizer is unsupported on macOS; it is on by default on Linux.
    if ! ASAN_OPTIONS="detect_leaks=${ORESHNEK_LSAN:-0}" \
         ctest --test-dir build-asan --output-on-failure; then
        echo "ASan/UBSan: FAILURES"; fail=1
    fi
else
    echo "ASan/UBSan: build failed"; fail=1
fi

# --- ThreadSanitizer ----------------------------------------------------------
if [ "$RUN_TSAN" = 1 ]; then
    section "ThreadSanitizer"
    if cmake -S . -B build-tsan -DORESHNEK_TSAN=ON -DORESHNEK_BUILD_EXAMPLES=OFF >/dev/null 2>&1 \
       && cmake --build build-tsan -j >/dev/null 2>&1; then
        if ! ctest --test-dir build-tsan --output-on-failure; then
            echo "TSan: FAILURES"; fail=1
        fi
    else
        echo "TSan: build failed"; fail=1
    fi
fi

# --- compile_commands.json for the static tools -------------------------------
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1

# --- clang-tidy (bugs, concurrency, CERT security, performance) ---------------
section "clang-tidy (static analysis)"
if have clang-tidy; then
    files=$(find src include -name '*.cpp' -o -name '*.h' | grep -v nlohmann_json)
    # shellcheck disable=SC2086
    clang-tidy -p build $files || { echo "clang-tidy: findings (see above)"; }
else
    echo "skipped — install with: brew install llvm   (or: apt-get install clang-tidy)"
fi

# --- cppcheck -----------------------------------------------------------------
section "cppcheck"
if have cppcheck; then
    cppcheck --enable=warning,performance,portability --std=c++20 --quiet \
             --suppress=missingIncludeSystem -I include src || true
else
    echo "skipped — install with: brew install cppcheck   (or: apt-get install cppcheck)"
fi

# --- valgrind (memory + fd leaks) — Linux only --------------------------------
section "valgrind (memcheck + fd leaks)"
if have valgrind; then
    cmake -S . -B build-dbg -DCMAKE_BUILD_TYPE=Debug -DORESHNEK_BUILD_EXAMPLES=OFF >/dev/null 2>&1
    cmake --build build-dbg --target security_test multipart_test db_test -j >/dev/null 2>&1
    for t in security_test multipart_test db_test; do
        echo "--- valgrind $t ---"
        valgrind --error-exitcode=1 --leak-check=full --track-fds=yes \
                 "build-dbg/$t" || { echo "valgrind $t: issues"; fail=1; }
    done
else
    echo "skipped — valgrind is Linux-only (on macOS rely on ASan above)."
fi

section "Summary"
if [ "$fail" = 0 ]; then
    echo "OK — sanitizer gate passed."
else
    echo "FAILURES — see sections above."
fi
exit "$fail"
