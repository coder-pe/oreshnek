# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(OreshnekServer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# --- Options -----------------------------------------------------------------
option(ORESHNEK_ASAN  "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ORESHNEK_TSAN  "Build with ThreadSanitizer" OFF)
option(ORESHNEK_BUILD_TESTS "Build the test suite" ON)

if(ORESHNEK_ASAN AND ORESHNEK_TSAN)
    message(FATAL_ERROR "ORESHNEK_ASAN and ORESHNEK_TSAN are mutually exclusive.")
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

find_package(SQLite3 REQUIRED)
if(SQLITE3_FOUND)
    message(STATUS "Found SQLite3: ${SQLite3_VERSION}")
endif()

# PostgreSQL client (libpq). On macOS, Homebrew's libpq is keg-only, so hint its
# location; on Linux it lives in the default search paths.
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
# Everything under src/ except the application entry point (main.cpp).
file(GLOB_RECURSE ORESHNEK_LIB_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER ORESHNEK_LIB_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(oreshnek STATIC ${ORESHNEK_LIB_SOURCES})
target_include_directories(oreshnek PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(oreshnek PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    PostgreSQL::PostgreSQL
    oreshnek_json
    oreshnek_sanitizers
)
target_compile_options(oreshnek PRIVATE -Wall -Wextra -pedantic)

# --- Application -------------------------------------------------------------
add_executable(oreshnek_server src/main.cpp)
target_link_libraries(oreshnek_server PRIVATE oreshnek oreshnek_sanitizers)
target_compile_options(oreshnek_server PRIVATE -Wall -Wextra -pedantic)

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

    add_executable(db_test tests/db_test.cpp)
    target_link_libraries(db_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(db_test PRIVATE -Wall -Wextra)
    add_test(NAME db_test COMMAND db_test)
    set_tests_properties(db_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(middleware_test tests/middleware_test.cpp)
    target_link_libraries(middleware_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(middleware_test PRIVATE -Wall -Wextra)
    add_test(NAME middleware_test COMMAND middleware_test)
    set_tests_properties(middleware_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    # PostgreSQL backend test: skips at runtime unless ORESHNEK_PG_TEST_DSN is set.
    add_executable(pg_test tests/pg_test.cpp)
    target_link_libraries(pg_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(pg_test PRIVATE -Wall -Wextra)
    add_test(NAME pg_test COMMAND pg_test)
    set_tests_properties(pg_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

    add_executable(tls_test tests/tls_test.cpp)
    target_link_libraries(tls_test PRIVATE oreshnek oreshnek_sanitizers)
    target_compile_options(tls_test PRIVATE -Wall -Wextra)
    add_test(NAME tls_test COMMAND tls_test)
    set_tests_properties(tls_test PROPERTIES ENVIRONMENT "${ORESHNEK_TEST_ENV}" TIMEOUT 60)

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
endif()
