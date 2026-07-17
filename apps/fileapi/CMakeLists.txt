# apps/fileapi/CMakeLists.txt
# The fileapi application links the framework library (oreshnek). It needs no
# database backend. Built as part of the tree when -DORESHNEK_BUILD_APPS=ON.
add_executable(fileapi main.cpp)
target_link_libraries(fileapi PRIVATE oreshnek oreshnek_sanitizers)
target_compile_options(fileapi PRIVATE -Wall -Wextra -pedantic)
