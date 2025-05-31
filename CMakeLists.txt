# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(OreshnekServer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # Good for shared libraries and some optimizations

# Define include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Find all source files using GLOB_RECURSE
# This recursively finds all .cpp files in src/ and its subdirectories
file(GLOB_RECURSE ORESHNEK_SOURCES
    "src/*.cpp"
    "src/*/*.cpp"
)

# Add the executable target
add_executable(oreshnek_server ${ORESHNEK_SOURCES})

# Link necessary libraries
# pthread is for std::thread, std::mutex, etc.
# m for math functions if used (e.g., in json parsing)
# rt for real-time extensions (optional, sometimes helps with epoll/timers)
target_link_libraries(oreshnek_server
    pthread
    m # For std::stod if JsonParser uses it, or other math ops
    # rt # Optional, might not be needed depending on system/kernel
)

# Enable maximum warnings and debug info
target_compile_options(oreshnek_server PRIVATE
    -Wall
    -Wextra
    -pedantic
    -g
    -O2 # Optimization level for performance
    -D_DEFAULT_SOURCE # To ensure certain POSIX features are available (e.g., `strerror_r`)
)

# Optional: Add compiler sanitizer flags for debugging memory issues (use for development, not production)
# target_compile_options(oreshnek_server PRIVATE
#     -fsanitize=address,undefined # AddressSanitizer, UndefinedBehaviorSanitizer
# )
# target_link_libraries(oreshnek_server -fsanitize=address,undefined)

# Optional: Configure installation
# install(TARGETS oreshnek_server DESTINATION bin)
# install(DIRECTORY include/oreshnek/ DESTINATION include)
