# Copyright (c) 2026 Christof Ruch. All rights reserved.
# Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase.

cmake_minimum_required(VERSION 3.14)

# kk only needs the MidiKraft session library and nlohmann/json, so it can be
# built on its own without JUCE or the Orm application.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(KnobKraftCLI LANGUAGES C CXX)
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
		# std::filesystem needs macOS 10.15
		set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
	endif()

	set(KNOBKRAFT_JSON_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/json" CACHE PATH "Path to the nlohmann/json source checkout")
	set(JSON_BuildTests OFF CACHE INTERNAL "")
	add_subdirectory("${KNOBKRAFT_JSON_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/json" EXCLUDE_FROM_ALL)
	add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../MidiKraft/session" "${CMAKE_CURRENT_BINARY_DIR}/MidiKraft-session" EXCLUDE_FROM_ALL)
	enable_testing()
endif()

add_library(kk-core STATIC
	src/Commands.cpp
	src/Commands.h
	src/KkClient.cpp
	src/KkClient.h
)
target_include_directories(kk-core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(kk-core PUBLIC midikraft::session nlohmann_json::nlohmann_json)
target_compile_features(kk-core PUBLIC cxx_std_20)

add_executable(kk src/main.cpp)
target_link_libraries(kk PRIVATE kk-core)

option(BUILD_KK_TESTS "Build the kk command-line client tests" OFF)
if(BUILD_KK_TESTS)
	add_executable(kk-tests tests/KkTests.cpp)
	target_link_libraries(kk-tests PRIVATE kk-core)
	add_test(NAME kk-tests COMMAND kk-tests)
endif()

if(MSVC)
	target_compile_options(kk-core PRIVATE /W4 /WX)
	target_compile_options(kk PRIVATE /W4 /WX)
	if(BUILD_KK_TESTS)
		target_compile_options(kk-tests PRIVATE /W4 /WX)
	endif()
else()
	target_compile_options(kk-core PRIVATE -Wall -Wextra -pedantic -Werror)
	target_compile_options(kk PRIVATE -Wall -Wextra -pedantic -Werror)
	if(BUILD_KK_TESTS)
		target_compile_options(kk-tests PRIVATE -Wall -Wextra -pedantic -Werror)
	endif()
endif()
