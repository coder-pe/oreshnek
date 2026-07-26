# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(OreshnekServer VERSION 0.1.0 LANGUAGES CXX)

# C++20 is preferred (enables real concepts for the database backend contract),
# but the framework also builds under C++17 for toolchains that lack a C++20
# compiler; pass -DORESHNEK_CXX_STANDARD=17 to target those.
set(ORESHNEK_CXX_STANDARD 20 CACHE STRING "C++ standard to build with (17 or 20)")
set_property(CACHE ORESHNEK_CXX_STANDARD PROPERTY STRINGS 17 20)
if(NOT ORESHNEK_CXX_STANDARD MATCHES "^(17|20)$")
    message(FATAL_ERROR "ORESHNEK_CXX_STANDARD must be 17 or 20 (got '${ORESHNEK_CXX_STANDARD}')")
endif()
set(CMAKE_CXX_STANDARD ${ORESHNEK_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# --- Options -----------------------------------------------------------------
option(ORESHNEK_ASAN  "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ORESHNEK_TSAN  "Build with ThreadSanitizer" OFF)
option(ORESHNEK_BUILD_TESTS "Build the test suite" ON)
option(ORESHNEK_BUILD_EXAMPLES "Build the example programs (examples/)" ON)
option(ORESHNEK_BUILD_APPS "Build the standalone applications (apps/)" OFF)
option(ORESHNEK_FUZZ "Build libFuzzer fuzz targets (needs a libFuzzer-capable clang)" OFF)

# Database backends are opt-in: enable only the one(s) a given project actually
# needs, so the build never depends on SQLite/PostgreSQL/Oracle client
# libraries it doesn't use. At least one must be enabled explicitly, e.g.:
#   cmake -B build -DORESHNEK_WITH_SQLITE=ON
#   cmake -B build -DORESHNEK_WITH_POSTGRES=ON -DORESHNEK_WITH_ORACLE=ON
option(ORESHNEK_WITH_SQLITE   "Build the SQLite3 database backend"   OFF)
option(ORESHNEK_WITH_POSTGRES "Build the PostgreSQL database backend" OFF)
option(ORESHNEK_WITH_ORACLE   "Build the Oracle database backend (OCI)" OFF)

if(ORESHNEK_ASAN AND ORESHNEK_TSAN)
    message(FATAL_ERROR "ORESHNEK_ASAN and ORESHNEK_TSAN are mutually exclusive.")
endif()
if(ORESHNEK_FUZZ AND ORESHNEK_TSAN)
    message(FATAL_ERROR "ORESHNEK_FUZZ and ORESHNEK_TSAN are mutually exclusive.")
endif()
if(NOT ORESHNEK_WITH_SQLITE AND NOT ORESHNEK_WITH_POSTGRES AND NOT ORESHNEK_WITH_ORACLE)
    message(FATAL_ERROR
        "No database backend enabled. Enable at least one of "
        "-DORESHNEK_WITH_SQLITE=ON, -DORESHNEK_WITH_POSTGRES=ON, -DORESHNEK_WITH_ORACLE=ON.")
endif()

# Sanitizer flags are applied to every target so the runtime is consistent.
add_library(oreshnek_sanitizers INTERFACE)
if(ORESHNEK_ASAN)
    message(STATUS "Sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer")
    target_compile_options(oreshnek_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(oreshnek_sanitizers INTERFACE -fsanitize=address,undefined)
elseif(ORESHNEK_TSAN)
    message(STATUS "Sanitizers: ThreadSanitizer")
    target_compile_options(oreshnek_sanitizers INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(oreshnek_sanitizers INTERFACE -fsanitize=thread)
endif()

# --- Dependencies ------------------------------------------------------------
find_package(Threads REQUIRED)

find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")
endif()

if(ORESHNEK_WITH_SQLITE)
    find_package(SQLite3 REQUIRED)
    if(SQLITE3_FOUND)
        message(STATUS "Found SQLite3: ${SQLite3_VERSION}")
    endif()
    # CMake's FindSQLite3 renamed its imported target from SQLite::SQLite3 to
    # SQLite3::SQLite3 in CMake 4.3 (keeping the old name as a deprecated
    # alias). On CMake < 4.3 (still the norm on most Linux distro package
    # repos) only SQLite::SQLite3 exists, so link against SQLite3::SQLite3
    # unconditionally would fail there. Shim per CMake's own docs so the rest
    # of this file can use one name regardless of CMake version.
    if(NOT TARGET SQLite3::SQLite3)
        add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
    endif()
endif()

# PostgreSQL client (libpq). On macOS, Homebrew's libpq is keg-only, so hint its
# location; on Linux it lives in the default search paths.
if(ORESHNEK_WITH_POSTGRES)
    if(APPLE AND NOT PostgreSQL_ROOT)
        execute_process(COMMAND brew --prefix libpq
                        OUTPUT_VARIABLE ORESHNEK_BREW_LIBPQ
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(ORESHNEK_BREW_LIBPQ)
            set(PostgreSQL_ROOT "${ORESHNEK_BREW_LIBPQ}")
        endif()
    endif()
    find_package(PostgreSQL REQUIRED)
    if(PostgreSQL_FOUND)
        message(STATUS "Found PostgreSQL (libpq): ${PostgreSQL_VERSION_STRING}")
    endif()
endif()

# Oracle client (OCI), via the Instant Client SDK. No stock CMake find module
# ships an "OCI" package, so headers/library are located directly.
# ORESHNEK_ORACLE_HOME defaults to this machine's Instant Client install; pass
# -DORESHNEK_ORACLE_HOME=/path/to/instantclient to point at a different one.
if(ORESHNEK_WITH_ORACLE)
    set(ORESHNEK_ORACLE_HOME "/home/miguel/oracle/instantclient_23_26"
        CACHE PATH "Path to the Oracle Instant Client (with its sdk/ subfolder)")

    find_path(ORACLE_INCLUDE_DIR oci.h
              HINTS "${ORESHNEK_ORACLE_HOME}/sdk/include" "$ENV{ORACLE_HOME}/sdk/include")
    find_library(ORACLE_LIBRARY NAMES clntsh
                 HINTS "${ORESHNEK_ORACLE_HOME}" "$ENV{ORACLE_HOME}")

    if(NOT ORACLE_INCLUDE_DIR OR NOT ORACLE_LIBRARY)
        message(FATAL_ERROR
            "Oracle Instant Client SDK not found (oci.h / libclntsh.so). "
            "Set -DORESHNEK_ORACLE_HOME=/path/to/instantclient_XX_YY.")
    endif()
    message(STATUS "Found Oracle Instant Client: ${ORESHNEK_ORACLE_HOME}")
endif()

# Compression: gzip via zlib (required), Brotli optional (auto-detected).
find_package(ZLIB REQUIRED)
message(STATUS "Found zlib: ${ZLIB_VERSION_STRING}")

find_path(BROTLI_INCLUDE_DIR brotli/encode.h
          HINTS /opt/homebrew/opt/brotli/include /usr/local/opt/brotli/include)
find_library(BROTLI_ENC NAMES brotlienc
             HINTS /opt/homebrew/opt/brotli/lib /usr/local/opt/brotli/lib)
find_library(BROTLI_COMMON NAMES brotlicommon
             HINTS /opt/homebrew/opt/brotli/lib /usr/local/opt/brotli/lib)
if(BROTLI_INCLUDE_DIR AND BROTLI_ENC AND BROTLI_COMMON)
    set(ORESHNEK_HAVE_BROTLI ON)
    message(STATUS "Found Brotli: ${BROTLI_ENC} (Content-Encoding: br enabled)")
else()
    set(ORESHNEK_HAVE_BROTLI OFF)
    message(STATUS "Brotli not found — only gzip available (install: brew install brotli)")
endif()

# nlohmann/json: prefer the vendored single-header (in nlohmann_json/, which is
# git-ignored), fall back to a system/CMake package. Marked SYSTEM so the
# header's own code does not trip our -Wall -Wextra -pedantic flags.
add_library(oreshnek_json INTERFACE)
set(ORESHNEK_NLOHMANN_SINGLE "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/single_include")
if(EXISTS "${ORESHNEK_NLOHMANN_SINGLE}/nlohmann/json.hpp")
    message(STATUS "Found nlohmann/json (vendored single-header)")
    target_include_directories(oreshnek_json SYSTEM INTERFACE "${ORESHNEK_NLOHMANN_SINGLE}")
else()
    find_package(nlohmann_json 3.2 REQUIRED)
    message(STATUS "Found nlohmann/json (system package)")
    target_link_libraries(oreshnek_json INTERFACE nlohmann_json::nlohmann_json)
endif()

# --- Framework library -------------------------------------------------------
# Everything under src/ except the application entry point (main.cpp) and any
# database backend sources whose CMake option is off, so the build never
# depends on a client library/headers it isn't using.
file(GLOB_RECURSE ORESHNEK_LIB_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER ORESHNEK_LIB_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

if(NOT ORESHNEK_WITH_SQLITE)
    list(FILTER ORESHNEK_LIB_SOURCES EXCLUDE REGEX ".*/Sqlite(Backend|Pool)\\.cpp$")
endif()
if(NOT ORESHNEK_WITH_POSTGRES)
    list(FILTER ORESHNEK_LIB_SOURCES EXCLUDE REGEX ".*/Pg(Backend|Pool)\\.cpp$")
endif()
if(NOT ORESHNEK_WITH_ORACLE)
    list(FILTER ORESHNEK_LIB_SOURCES EXCLUDE REGEX ".*/Oracle(Backend|Pool)\\.cpp$")
endif()

add_library(oreshnek STATIC ${ORESHNEK_LIB_SOURCES})
target_include_directories(oreshnek PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(oreshnek PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    oreshnek_json
    oreshnek_sanitizers
)

if(ORESHNEK_WITH_SQLITE)
    target_link_libraries(oreshnek PUBLIC SQLite3::SQLite3)
    target_compile_definitions(oreshnek PUBLIC ORESHNEK_WITH_SQLITE)
endif()
if(ORESHNEK_WITH_POSTGRES)
    target_link_libraries(oreshnek PUBLIC PostgreSQL::PostgreSQL)
    target_compile_definitions(oreshnek PUBLIC ORESHNEK_WITH_POSTGRES)
endif()
if(ORESHNEK_WITH_ORACLE)
    target_include_directories(oreshnek SYSTEM PUBLIC ${ORACLE_INCLUDE_DIR})
    target_link_libraries(oreshnek PUBLIC ${ORACLE_LIBRARY})
    target_compile_definitions(oreshnek PUBLIC ORESHNEK_WITH_ORACLE)
endif()
if(ORESHNEK_HAVE_BROTLI)
    target_include_directories(oreshnek SYSTEM PRIVATE ${BROTLI_INCLUDE_DIR})
    target_link_libraries(oreshnek PUBLIC ${BROTLI_ENC} ${BROTLI_COMMON})
    target_compile_definitions(oreshnek PRIVATE ORESHNEK_HAVE_BROTLI)
endif()
target_compile_options(oreshnek PRIVATE -Wall -Wextra -pedantic)

# When fuzzing, instrument the whole library with coverage + ASan/UBSan so the
# fuzz target (which provides its own main via -fsanitize=fuzzer) gets edge
# coverage and memory checking through the parser and its dependencies.
if(ORESHNEK_FUZZ)
    message(STATUS "Fuzzing: libFuzzer coverage + ASan/UBSan on the oreshnek library")
    target_compile_options(oreshnek PUBLIC
        -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(oreshnek PUBLIC -fsanitize=address,undefined)
endif()

# The framework builds a library only; there is no application entry point in
# src/. Runnable programs live in examples/ (demos) and apps/ (real apps).

# --- Examples ----------------------------------------------------------------
if(ORESHNEK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# --- Applications ------------------------------------------------------------
# Standalone apps built on the framework (each in its own directory with its own
# CMakeLists linking the oreshnek library).
if(ORESHNEK_BUILD_APPS)
    add_subdirectory(apps/fileapi)
endif()

# --- Tests -------------------------------------------------------------------
if(ORESHNEK_BUILD_TESTS)
    enable_testing()

    set(ORESHNEK_TEST_ENV
        "ASAN_OPTIONS=abort_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;TSAN_OPTIONS=halt_on_error=1")

    add_executable(integration_test tests/integration_test.cpp)
    target_link_libraries(integration_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(integration_test PRIVATE -Wall -Wextra)
    add_test(NAME integration_test COMMAND integration_test)
    # Fail fast and surface sanitizer reports as test failures.
    set_tests_properties(integration_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(security_test tests/security_test.cpp)
    target_link_libraries(security_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(security_test PRIVATE -Wall -Wextra)
    add_test(NAME security_test COMMAND security_test)
    set_tests_properties(security_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(upload_test tests/upload_test.cpp)
    target_link_libraries(upload_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(upload_test PRIVATE -Wall -Wextra)
    add_test(NAME upload_test COMMAND upload_test)
    set_tests_properties(upload_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(multipart_test tests/multipart_test.cpp)
    target_link_libraries(multipart_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(multipart_test PRIVATE -Wall -Wextra)
    add_test(NAME multipart_test COMMAND multipart_test)
    set_tests_properties(multipart_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(lifecycle_test tests/lifecycle_test.cpp)
    target_link_libraries(lifecycle_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(lifecycle_test PRIVATE -Wall -Wextra)
    add_test(NAME lifecycle_test COMMAND lifecycle_test)
    set_tests_properties(lifecycle_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    if(ORESHNEK_WITH_SQLITE)
        add_executable(db_test tests/db_test.cpp)
        target_link_libraries(db_test PRIVATE oreshnek oreshnek_sanitizers)
        target_compile_options(db_test PRIVATE -Wall -Wextra)
        add_test(NAME db_test COMMAND db_test)
        set_tests_properties(db_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)
    endif()

    add_executable(middleware_test tests/middleware_test.cpp)
    target_link_libraries(middleware_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(middleware_test PRIVATE -Wall -Wextra)
    add_test(NAME middleware_test COMMAND middleware_test)
    set_tests_properties(middleware_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    # PostgreSQL backend test: skips at runtime unless ORESHNEK_PG_TEST_DSN is set.
    if(ORESHNEK_WITH_POSTGRES)
        add_executable(pg_test tests/pg_test.cpp)
        target_link_libraries(pg_test PRIVATE oreshnek oreshnek_sanitizers)
        target_compile_options(pg_test PRIVATE -Wall -Wextra)
        add_test(NAME pg_test COMMAND pg_test)
        set_tests_properties(pg_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)
    endif()

    # Oracle backend test: skips at runtime unless ORESHNEK_ORACLE_TEST_DSN is set.
    if(ORESHNEK_WITH_ORACLE)
        add_executable(oracle_test tests/oracle_test.cpp)
        target_link_libraries(oracle_test PRIVATE oreshnek oreshnek_sanitizers)
        target_compile_options(oracle_test PRIVATE -Wall -Wextra)
        add_test(NAME oracle_test COMMAND oracle_test)
        set_tests_properties(oracle_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)
    endif()

    add_executable(tls_test tests/tls_test.cpp)
    target_link_libraries(tls_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(tls_test PRIVATE -Wall -Wextra)
    add_test(NAME tls_test COMMAND tls_test)
    set_tests_properties(tls_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(compression_test tests/compression_test.cpp)
    target_link_libraries(compression_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(compression_test PRIVATE -Wall -Wextra)
    add_test(NAME compression_test COMMAND compression_test)
    set_tests_properties(compression_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(rate_limit_test tests/rate_limit_test.cpp)
    target_link_libraries(rate_limit_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(rate_limit_test PRIVATE -Wall -Wextra)
    add_test(NAME rate_limit_test COMMAND rate_limit_test)
    set_tests_properties(rate_limit_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(metrics_test tests/metrics_test.cpp)
    target_link_libraries(metrics_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(metrics_test PRIVATE -Wall -Wextra)
    add_test(NAME metrics_test COMMAND metrics_test)
    set_tests_properties(metrics_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    # Deterministic replay of the fuzz corpus + saved crash reproducers. Uses the
    # same fuzz body as the libFuzzer target but no fuzzer runtime, so it runs on
    # any toolchain (incl. Apple clang) and guards against regressions in ctest.
    add_executable(fuzz_replay_test tests/fuzz/fuzz_replay.cpp)
    target_link_libraries(fuzz_replay_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(fuzz_replay_test PRIVATE -Wall -Wextra)
    target_compile_definitions(fuzz_replay_test PRIVATE
        ORESHNEK_FUZZ_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz")
    add_test(NAME fuzz_replay_test COMMAND fuzz_replay_test)
    set_tests_properties(fuzz_replay_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)
endif()

# --- Fuzzing (libFuzzer) -----------------------------------------------------
# Requires a libFuzzer-capable clang. On macOS, Apple clang has no fuzzer
# runtime; configure with Homebrew LLVM, e.g.:
#   cmake -B build-fuzz -DORESHNEK_FUZZ=ON \
#     -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
if(ORESHNEK_FUZZ)
    add_executable(fuzz_http_parser tests/fuzz/fuzz_http_parser.cpp)
    target_link_libraries(fuzz_http_parser PRIVATE oreshnek)
    target_compile_options(fuzz_http_parser PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1)
    target_link_options(fuzz_http_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
