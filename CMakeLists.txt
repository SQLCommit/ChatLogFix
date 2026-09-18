####################################################################################################
# chatlogfix - Ashita v4 plugin (32-bit).
#
# Fills the expanded chat log (fulllog) to the full height of its window instead of stopping halfway.
#
# Configure (from a Windows box, x86 toolset; ASHITA4_SDK_PATH must point at a folder with Ashita.h):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#
# Output: build/Release/chatlogfix.dll  ->  copy to Ashita-v4beta-main/plugins/
####################################################################################################
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

# Do NOT enable Safe-SEH -- the legacy Direct3D8 import libs are not /SAFESEH-compatible.

# Emit a linker MAP so a crash address can be turned into a function name instead of a guess.
target_link_options(chatlogfix PRIVATE "/MAP" "/MAPINFO:EXPORTS")
