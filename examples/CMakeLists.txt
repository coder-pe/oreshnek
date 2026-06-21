# examples/CMakeLists.txt
# Each example is a standalone program linked against the framework library.
# common.h (startup/shutdown helper) is resolved relative to each source file.

set(ORESHNEK_EXAMPLES
    01_hello_json
    02_middleware
    03_rest_crud_db
    04_static_files
    05_production
    06_video_platform)

foreach(example ${ORESHNEK_EXAMPLES})
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE oreshnek)
    target_compile_options(${example} PRIVATE -Wall -Wextra)
endforeach()
