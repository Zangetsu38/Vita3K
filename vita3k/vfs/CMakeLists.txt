add_library(
    vfs
    STATIC
    include/vfs/mounts.h
    src/mounts.cpp
)

target_include_directories(vfs PUBLIC include)
target_link_libraries(vfs PUBLIC pfsmgr io)
target_link_libraries(vfs PRIVATE kernel)
