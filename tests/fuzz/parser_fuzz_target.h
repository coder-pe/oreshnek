// tests/fuzz/parser_fuzz_target.h
//
// Shared fuzz body for the HTTP parser. It is used by two entry points:
//   * fuzz_http_parser.cpp — the libFuzzer target (needs a libFuzzer-capable
//     clang; on macOS that is Homebrew LLVM, since Apple clang ships no fuzzer
//     runtime).
//   * fuzz_replay.cpp — a deterministic replay of the seed corpus and saved
//     crash reproducers, wired into ctest so regressions stay guarded even
//     where libFuzzer is unavailable.
//
// The body does more than "does not crash": it asserts parser invariants, so a
// contract violation aborts (which libFuzzer/ASan report as a finding).
#ifndef ORESHNEK_TESTS_FUZZ_PARSER_TARGET_H
#define ORESHNEK_TESTS_FUZZ_PARSER_TARGET_H

#include "oreshnek/http/HttpParser.h"
#include "oreshnek/http/HttpRequest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace oreshnek_fuzz {

using Oreshnek::Http::HttpParser;
using Oreshnek::Http::HttpRequest;
using Oreshnek::Http::ParsingState;

// Abort (→ a crash report) when a parser invariant is broken. Not compiled out
// in release, unlike assert().
inline void expect(bool cond) {
    if (!cond) std::abort();
}

inline void check_after_parse(const HttpParser& parser, bool complete,
                              std::size_t consumed, std::size_t buf_size) {
    // The parser must never claim to have consumed more than it was handed.
    expect(consumed <= buf_size);
    const ParsingState st = parser.get_state();
    // Completion is reported iff the parser reached the COMPLETE state, and a
    // complete request always makes forward progress.
    if (complete) {
        expect(st == ParsingState::COMPLETE);
        expect(consumed > 0);
    } else {
        expect(st != ParsingState::COMPLETE);
    }
}

// Mode 1: parse the whole buffer in a single pass, then exercise the accessors
// and the owned-copy rebasing path (make_owned) on a completed request.
inline void parse_once(const std::uint8_t* data, std::size_t size) {
    std::vector<char> buf(data, data + size);  // mutable: the chunked path
                                               // compacts the body in place.
    HttpParser parser;
    HttpRequest req;
    std::size_t consumed = 0;
    std::string_view view(buf.data(), buf.size());
    const bool complete = parser.parse_request(view, consumed, req);
    check_after_parse(parser, complete, consumed, buf.size());

    if (complete) {
        (void)req.method();
        (void)req.path();
        (void)req.version();
        (void)req.body();
        (void)req.header("host");
        (void)req.query("q");
        // Detach into owned storage: fuzzes rebase_views over the consumed bytes.
        req.make_owned(buf.data(), consumed);
        (void)req.path();
        (void)req.body();
    }
}

// Mode 2: mirror Connection::parse_next — bytes trickle in and the whole buffer
// is re-parsed from a clean state each pass; a completed request is consumed so
// the tail (pipelining) keeps flowing. The first byte seeds the chunk size.
inline void parse_incremental(const std::uint8_t* data, std::size_t size) {
    const std::size_t step = static_cast<std::size_t>(data[0]) + 1;  // 1..256
    const std::uint8_t* stream = data + 1;
    const std::size_t stream_size = size - 1;

    std::vector<char> buf;
    HttpParser parser;
    std::size_t fed = 0;
    int guard = 0;
    while (fed < stream_size && guard++ < 200000) {
        const std::size_t take = std::min(step, stream_size - fed);
        buf.insert(buf.end(), stream + fed, stream + fed + take);
        fed += take;

        parser.reset();
        HttpRequest req;
        std::size_t consumed = 0;
        std::string_view view(buf.data(), buf.size());
        const bool complete = parser.parse_request(view, consumed, req);
        check_after_parse(parser, complete, consumed, buf.size());

        if (parser.get_state() == ParsingState::ERROR) break;  // terminal state
        if (complete) {
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
    }
}

// Route on the low bit of the first byte to cover both modes over a campaign.
inline void run(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return;
    if (data[0] & 1u) {
        parse_incremental(data, size);
    } else {
        parse_once(data + 1, size - 1);
    }
}

}  // namespace oreshnek_fuzz

#endif  // ORESHNEK_TESTS_FUZZ_PARSER_TARGET_H
