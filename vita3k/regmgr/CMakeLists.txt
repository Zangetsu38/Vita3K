add_library(
	regmgr
	STATIC
	include/regmgr/functions.h
	include/regmgr/state.h
	include/regmgr/types.h
	src/regmgr.cpp
)

target_include_directories(regmgr PUBLIC include)
target_include_directories(regmgr PRIVATE)
target_link_libraries(regmgr PUBLIC util)
if(WIN32)
	target_link_libraries(regmgr PRIVATE advapi32)
endif()

if(NOT ANDROID)
	add_executable(regmgr-tests tests/regmgr_tests.cpp)
	target_link_libraries(regmgr-tests PRIVATE regmgr googletest)
	add_test(NAME regmgr COMMAND regmgr-tests)
endif()

