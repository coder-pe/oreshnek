// tests/fuzz/fuzz_replay.cpp
//
// Deterministic replay of the seed corpus and any saved crash reproducers
// through the same fuzz body as the libFuzzer target. Wired into ctest so
// regressions are guarded on every build — including toolchains without
// libFuzzer (e.g. Apple clang). It parses every file under
// $ORESHNEK_FUZZ_DIR/{corpus,regressions}; a broken invariant aborts the run.
#include "parser_fuzz_target.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace {
int replay_dir(const fs::path& dir) {
    if (!fs::exists(dir)) return 0;
    int files = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        oreshnek_fuzz::run(bytes.data(), bytes.size());  // aborts on a finding
        ++files;
    }
    return files;
}
}  // namespace

int main() {
    const fs::path base = ORESHNEK_FUZZ_DIR;
    int files = 0;
    files += replay_dir(base / "corpus");
    files += replay_dir(base / "regressions");
    std::cout << "[OK] fuzz replay: " << files
              << " input(s) parsed without crash or invariant violation\n";
    return 0;
}
