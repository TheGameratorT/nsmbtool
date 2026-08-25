cmake_minimum_required(VERSION 3.20)

project(nsmbtool VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(NSMBTOOL_BUILD_TESTS "Build unit tests" OFF)

# yaml-cpp is the only library. It reads nsmbref.lock, and it is what will read
# NCPatcher's dumps when the generators land -- JSON is a subset of YAML 1.2, so
# one parser covers both and the two programs stay free of a shared dependency.
#
# Preferred from the system: a build-time download is unusable offline and
# unusable for distro packaging. NSMBTOOL_USE_SYSTEM_DEPS turns a missing dependency
# into a configure error instead of a silent fetch.
option(NSMBTOOL_USE_SYSTEM_DEPS "Require dependencies from the system, never fetch them" OFF)

if (NSMBTOOL_USE_SYSTEM_DEPS)
	find_package(yaml-cpp REQUIRED)
else()
	find_package(yaml-cpp QUIET)
endif()

if (NOT yaml-cpp_FOUND)
	message(STATUS "yaml-cpp not found on the system, fetching it")
	include(FetchContent)
	set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(yaml-cpp
		GIT_REPOSITORY "https://github.com/jbeder/yaml-cpp.git"
		GIT_TAG 0.8.0
		GIT_SHALLOW ON
	)
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
	FetchContent_MakeAvailable(yaml-cpp)

	# 0.8.0's emitterutils.cpp names uint16_t without including <cstdint>, which
	# GCC 13 stopped pulling in transitively. The fetch is the fallback for
	# machines with no system package -- and the only path a MinGW cross-build
	# has -- so it has to build rather than being left to a future release.
	if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		target_compile_options(yaml-cpp PRIVATE -include cstdint)
	endif()
endif()

if (TARGET yaml-cpp::yaml-cpp)
	set(YAML_CPP_TARGET yaml-cpp::yaml-cpp)
else()
	set(YAML_CPP_TARGET yaml-cpp)
endif()

if (MINGW)
	set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")
endif()

# Listed rather than globbed, so that the build does not depend on the state of
# the filesystem at configure time.
set(SOURCES
	source/main.cpp
	source/lockfile.cpp
	source/log.cpp
	source/process.cpp
	source/project.cpp
	source/reference.cpp
	source/store.cpp
	source/unicode.cpp
)

add_executable(nsmbtool ${SOURCES})

target_compile_features(nsmbtool PRIVATE cxx_std_20)
set_target_properties(nsmbtool PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(nsmbtool PRIVATE ${YAML_CPP_TARGET})

if (WIN32)
	# CommandLineToArgvW, which is how the arguments are recovered as UTF-16
	# before the ANSI code page has had a chance to mangle them.
	target_link_libraries(nsmbtool PRIVATE shell32)
endif()

if (MSVC)
	target_compile_options(nsmbtool PRIVATE /W4 /utf-8)
else()
	target_compile_options(nsmbtool PRIVATE -Wall -Wextra)
endif()

install(TARGETS nsmbtool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if (NSMBTOOL_BUILD_TESTS)
	enable_testing()

	# The store and reference tests drive the real git, in a scratch store and a
	# scratch repository, because the interesting failures here are all in what
	# git actually does -- a mocked git would only confirm that the arguments
	# were spelled the way the mock expected.
	add_executable(lockfile_test
		tests/lockfile_test.cpp
		source/lockfile.cpp
		source/log.cpp
		source/unicode.cpp
	)
	target_compile_features(lockfile_test PRIVATE cxx_std_20)
	target_link_libraries(lockfile_test PRIVATE ${YAML_CPP_TARGET})
	add_test(NAME lockfile COMMAND lockfile_test)

	add_executable(project_test
		tests/project_test.cpp
		source/lockfile.cpp
		source/log.cpp
		source/process.cpp
		source/project.cpp
		source/unicode.cpp
	)
	target_compile_features(project_test PRIVATE cxx_std_20)
	target_link_libraries(project_test PRIVATE ${YAML_CPP_TARGET})
	add_test(NAME project COMMAND project_test)

	add_executable(process_test
		tests/process_test.cpp
		source/process.cpp
		source/unicode.cpp
	)
	target_compile_features(process_test PRIVATE cxx_std_20)
	if (WIN32)
		target_link_libraries(process_test PRIVATE shell32)
	endif()
	add_test(NAME process COMMAND process_test)

	add_executable(store_test
		tests/store_test.cpp
		source/lockfile.cpp
		source/log.cpp
		source/process.cpp
		source/store.cpp
		source/unicode.cpp
	)
	target_compile_features(store_test PRIVATE cxx_std_20)
	target_link_libraries(store_test PRIVATE ${YAML_CPP_TARGET})
	add_test(NAME store COMMAND store_test)
endif()
