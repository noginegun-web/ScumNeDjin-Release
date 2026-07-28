cmake_minimum_required(VERSION 3.22)
project(ScumNedjinManagedBridgeMod LANGUAGES CXX)

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

add_library(ScumNedjinManagedBridgeMod SHARED
    src/dllmain.cpp
)

target_include_directories(ScumNedjinManagedBridgeMod PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../SCUMTraderManager/include"
)
target_link_libraries(ScumNedjinManagedBridgeMod PRIVATE
    "${SCUM_NEDJIN_UE4SS_IMPORT_LIB}"
)
target_compile_definitions(ScumNedjinManagedBridgeMod PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

set_target_properties(ScumNedjinManagedBridgeMod PROPERTIES
    OUTPUT_NAME "main"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Mods/ScumNedjinManagedBridgeMod/dlls"
)

add_custom_command(TARGET ScumNedjinManagedBridgeMod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Mods/ScumNedjinManagedBridgeMod"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/Mods/ScumNedjinManagedBridgeMod/enabled.txt"
)
