# chatlogfix - Ashita v4 plugin (32-bit).
# Fills the expanded chat log and serializes chat-buffer access.
cmake_minimum_required(VERSION 3.22)

project(chatlogfix LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(AshitaSDK REQUIRED)
ashita_sdk_initialize()

add_library(chatlogfix MODULE
    src/chatlogfix.cpp
    src/chatlogfix.hpp
    src/chatserial.cpp
    src/chatserial.hpp
    src/chatserial_cave.hpp
    src/exports.def
)

set_target_properties(chatlogfix PROPERTIES
    PREFIX ""
    OUTPUT_NAME chatlogfix
    SUFFIX ".dll"
)

# Attach SDK includes/libs and the recommended compiler/linker defaults.
ashita_sdk_attach(chatlogfix)
ashita_sdk_set_compiler_defaults(chatlogfix)
ashita_sdk_set_linker_defaults(chatlogfix)
ashita_sdk_set_compiler_warn_level(chatlogfix 3)

# Legacy Direct3D8 import libraries are not Safe-SEH compatible.

# Emit a linker map for crash diagnostics.
target_link_options(chatlogfix PRIVATE "/MAP" "/MAPINFO:EXPORTS")
