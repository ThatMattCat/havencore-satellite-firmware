# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS
    components
    )
add_compile_options(-fdiagnostics-color=always -Wno-ignored-qualifiers)

# IDF reads PROJECT_VER from version.txt (when present) and bakes it into
# esp_app_desc_t. scripts/bump_version.sh rewrites version.txt on every
# `make build` so dev rebuilds produce a unique version — that's what
# makes Settings -> Update Firmware actually pull a fresh build instead
# of strcmp'ing the two `-dirty` strings as equal. Declaring version.txt
# as a configure-dep means a fresh write forces re-configure, which
# re-bakes esp_app_desc.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/version.txt")

project(havencore_satellite)

