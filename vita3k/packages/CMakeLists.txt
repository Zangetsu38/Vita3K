add_library(packages STATIC
            src/appmeta.cpp
            src/exfat.cpp
            src/license.cpp
            src/pkg.cpp
            src/pup.cpp
            src/sce_utils.cpp
            src/sfo.cpp
            src/vci.cpp
            include/packages/exfat.h
            include/packages/license.h
            include/packages/functions.h
            include/packages/pkg.h
            include/packages/sce_types.h
            include/packages/sfo.h
            include/packages/vci.h
)
target_include_directories(packages PUBLIC include)
target_link_libraries(packages PUBLIC emuenv util pfsmgr)
target_link_libraries(packages PRIVATE config crypto emuenv FAT16 io miniz libzRIF vfs vita-toolchain)

if(NOT ANDROID)
    add_executable(savedata-identity-tests tests/savedata_identity_tests.cpp)
    target_link_libraries(savedata-identity-tests PRIVATE app config packages io regmgr googletest)
    add_test(NAME savedata-identity COMMAND savedata-identity-tests)
endif()
