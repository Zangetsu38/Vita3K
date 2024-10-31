add_library(
	v3kn
	STATIC
	include/v3kn/account.h
	include/v3kn/activity.h
	include/v3kn/friend.h
	include/v3kn/messages.h
	include/v3kn/state.h
	include/v3kn/storage.h
	src/account.cpp
	src/activity.cpp
	src/friend.cpp
	src/messages.cpp
	src/storage.cpp
)

target_include_directories(v3kn PUBLIC include)
target_link_libraries(v3kn PUBLIC gui emuenv util packages)
target_link_libraries(v3kn PRIVATE pugixml::pugixml miniz nlohmann_json::nlohmann_json stb)
