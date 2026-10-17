add_library(
	regmgr
	STATIC
	include/regmgr/functions.h
	include/regmgr/state.h
	include/regmgr/types.h
	src/regmgr.cpp
)

target_include_directories(regmgr PUBLIC include)
target_link_libraries(regmgr PRIVATE kernel)
target_link_libraries(regmgr PUBLIC util mem)
if(WIN32)
	target_link_libraries(regmgr PRIVATE advapi32)
endif()
