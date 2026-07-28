cmake_minimum_required(VERSION 3.24)
project(ScumWardenNative LANGUAGES CXX)

option(SCUM_NEDJIN_PUBLIC_REQUIRE_CONFIGURED_API_KEY "Fail closed until a public host package has a unique local API key" ON)

add_library(version SHARED
    src/dllmain.cpp
    src/config_key.cpp
    src/http_server.cpp
    src/path_utils.cpp
    src/real_version.cpp
    src/bridge_client.cpp
    src/sqlite_db.cpp
    src/managed_loader.cpp
    src/managed_host_api.cpp
)

target_compile_features(version PRIVATE cxx_std_20)
target_compile_definitions(version PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(SCUM_NEDJIN_PUBLIC_REQUIRE_CONFIGURED_API_KEY)
    target_compile_definitions(version PRIVATE SCUM_NEDJIN_PUBLIC_REQUIRE_CONFIGURED_API_KEY)
endif()
target_include_directories(version PRIVATE include)
target_link_libraries(version PRIVATE ws2_32 winhttp bcrypt)

set_target_properties(version PROPERTIES
    OUTPUT_NAME "version"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)
