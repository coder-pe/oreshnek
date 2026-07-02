// tests/fuzz/fuzz_http_parser.cpp
//
// libFuzzer entry point for the HTTP parser. Build with a libFuzzer-capable
// clang (see docs/LOAD_AND_FUZZ_PLAN.md); the shared body lives in
// parser_fuzz_target.h so the deterministic replay can reuse it.
#include "parser_fuzz_target.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    oreshnek_fuzz::run(data, size);
    return 0;
}
