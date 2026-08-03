cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	project(win-openxr LANGUAGES CXX)

	if(NOT OBS_SOURCE_DIR OR NOT OBS_IMPORT_LIBRARY)
		message(FATAL_ERROR
			"Standalone builds require OBS_SOURCE_DIR and OBS_IMPORT_LIBRARY")
	endif()

	set(OBS_RELEASE_CANDIDATE 0)
	set(OBS_BETA 0)
	file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
	configure_file("${OBS_SOURCE_DIR}/libobs/obsconfig.h.in"
		"${CMAKE_BINARY_DIR}/config/obsconfig.h" @ONLY)

	add_library(libobs SHARED IMPORTED)
	set_target_properties(libobs PROPERTIES
		IMPORTED_IMPLIB "${OBS_IMPORT_LIBRARY}"
		INTERFACE_INCLUDE_DIRECTORIES
			"${OBS_SOURCE_DIR}/libobs;${CMAKE_BINARY_DIR}/config")
endif()

set(win-openxr_SOURCES
	win-openxr.cpp)

add_library(win-openxr MODULE
	${win-openxr_SOURCES})
target_compile_features(win-openxr PRIVATE cxx_std_17)
target_include_directories(win-openxr PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../shared)
target_link_libraries(win-openxr
	libobs)

if(MSVC)
	# Keep the installed-binary hash stable across no-op rebuilds so the
	# Control Center only reports an update when the plugin actually changed.
	target_compile_options(win-openxr PRIVATE /Brepro)
	target_link_options(win-openxr PRIVATE /Brepro)
endif()

if(COMMAND install_obs_plugin_with_data)
	install_obs_plugin_with_data(win-openxr data)
endif()
