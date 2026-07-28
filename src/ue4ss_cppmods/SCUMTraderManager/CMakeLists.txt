cmake_minimum_required(VERSION 3.22)
project(SCUMTraderManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# UE4SS binaries and import libraries are intentionally not redistributed by
# this repository. Point this cache variable at UE4SS.lib from a locally built
# SDK that matches the server's UE4SS runtime and x64 toolchain.
set(SCUM_NEDJIN_UE4SS_IMPORT_LIB "" CACHE FILEPATH
    "Absolute path to the matching UE4SS.lib import library")
if(NOT SCUM_NEDJIN_UE4SS_IMPORT_LIB OR NOT EXISTS "${SCUM_NEDJIN_UE4SS_IMPORT_LIB}")
    message(FATAL_ERROR
        "Set SCUM_NEDJIN_UE4SS_IMPORT_LIB to UE4SS.lib from a matching local UE4SS SDK.")
endif()

add_library(SCUMTraderManager SHARED
    src/dllmain.cpp
    src/SCUMTraderManager.cpp
)

target_include_directories(SCUMTraderManager PRIVATE include)
target_link_libraries(SCUMTraderManager PRIVATE "${SCUM_NEDJIN_UE4SS_IMPORT_LIB}" winhttp)

target_compile_definitions(SCUMTraderManager PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

set_target_properties(SCUMTraderManager PROPERTIES
    OUTPUT_NAME "main"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Mods/SCUMTraderManager/dlls"
)

add_custom_command(TARGET SCUMTraderManager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Mods/SCUMTraderManager"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/Mods/SCUMTraderManager/enabled.txt"
)
