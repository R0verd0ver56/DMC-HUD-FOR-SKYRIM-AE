cmake_minimum_required(VERSION 3.21)

project(BonoDMCPort VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT (matches the vcpkg x64-windows-static triplet) so users need no redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# CommonLibSSE-NG is cloned into extern/ by the GitHub workflow (see .github/workflows/build.yml).
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG/CMakeLists.txt")
	message(FATAL_ERROR "extern/CommonLibSSE-NG is missing. The GitHub workflow clones it; for a local build run:\n"
		"  git clone https://github.com/CharmedBaryon/CommonLibSSE-NG.git extern/CommonLibSSE-NG\n"
		"  git -C extern/CommonLibSSE-NG checkout b93280e")
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

function(bono_plugin NAME SRC)
	add_commonlibsse_plugin(${NAME}
		AUTHOR "Bono (AE/SE port)"
		VERSION ${PROJECT_VERSION}
		SOURCES ${SRC})
	target_include_directories(${NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
	if(MSVC)
		target_compile_options(${NAME} PRIVATE /utf-8 /permissive- /Zc:__cplusplus /MP /W3)
	endif()
	set_target_properties(${NAME} PROPERTIES OUTPUT_NAME ${NAME})
endfunction()

bono_plugin(BonoDMCRank rank/main.cpp)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hud/main.cpp")
	bono_plugin(BonoDMCHUD hud/main.cpp)
endif()
