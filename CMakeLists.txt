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

# --- Framework library -------------------------------------------------------
# Everything under src/ except the application entry point (main.cpp).
file(GLOB_RECURSE ORESHNEK_LIB_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER ORESHNEK_LIB_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(oreshnek STATIC ${ORESHNEK_LIB_SOURCES})
target_include_directories(oreshnek PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(oreshnek PUBLIC
    Threads::Threads
    OpenSSL::Crypto
    SQLite::SQLite3
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
endif()
