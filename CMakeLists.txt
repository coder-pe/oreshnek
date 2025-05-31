# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(OreshnekServer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Define include directories (para tus propios headers)
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

file(GLOB_RECURSE ORESHNEK_SOURCES
    "src/*.cpp"
    "src/*/*.cpp"
)

# --- Find Threads ---
find_package(Threads REQUIRED) # <<< --- AÑADE ESTA LÍNEA

# --- Find OpenSSL ---
find_package(OpenSSL REQUIRED)
if (OPENSSL_FOUND)
    message(STATUS "Found OpenSSL libraries: ${OpenSSL_LIBRARIES}")
    message(STATUS "Found OpenSSL include dir: ${OpenSSL_INCLUDE_DIR}")
    # No necesitas include_directories(${OPENSSL_INCLUDE_DIR}) si usas imported targets
    # No necesitas set(OPENSSL_LIBRARIES ...) si usas imported targets
else()
    message(FATAL_ERROR "OpenSSL not found. Please install it or specify its path.")
endif()

# --- Find SQLite3 ---
find_package(SQLite3 REQUIRED)
if (SQLITE3_FOUND)
    message(STATUS "Found SQLite3 library: ${SQLite3_LIBRARY}") # O SQLite3_LIBRARIES
    message(STATUS "Found SQLite3 include dir: ${SQLite3_INCLUDE_DIR}") # 
    # No necesitas include_directories(${SQLITE3_INCLUDE_DIR}) si usas imported targets
else()
    message(FATAL_ERROR "SQLite3 not found. Please install it or specify its path.") # 
endif()

add_executable(oreshnek_server ${ORESHNEK_SOURCES})

# Link necessary libraries usando imported targets
target_link_libraries(oreshnek_server PRIVATE
    Threads::Threads   # Para pthreads
    m                  # Para la biblioteca matemática, si es necesaria
    OpenSSL::Crypto    # Para funciones criptográficas de OpenSSL (SHA, HMAC, etc.)
                       # Si usas funciones SSL/TLS directamente, añade OpenSSL::SSL también.
    SQLite::SQLite3    # Para SQLite3
)

target_compile_options(oreshnek_server PRIVATE
    -Wall
    -Wextra
    -pedantic
    -g
    -O2
    -D_DEFAULT_SOURCE
)

# Opcional: Configuraciones de instalación y sanitizers como los tenías.