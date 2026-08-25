#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
cmake_minimum_required(VERSION 3.25)

project(
  stm32-embedded-demo
  VERSION 0.1.0
  LANGUAGES CXX C ASM
  )

include(cmake/get_cpm.cmake)
cpmaddpackage("gh:intel/cicd-repo-infrastructure#dev")

add_versioned_package("gh:intel/cpp-baremetal-concurrency#b8a486a")
add_versioned_package("gh:intel/cpp-std-extensions#0f91ae4")
add_versioned_package("gh:intel/cpp-baremetal-senders-and-receivers#3e3c8aa")
add_versioned_package("gh:intel/generic-register-operation-optimizer#2e45cc2")
add_versioned_package("gh:caisselabs/stm32-groov#6769da8")
add_versioned_package("gh:intel/compile-time-init-build#f9b4cc5")



if(NOT CMAKE_CROSSCOMPILING)
  # src/i2c is a work in progress and has no CMakeLists.txt yet; skip it rather
  # than failing the whole host configure. Remove the guard once it has one.
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/i2c/CMakeLists.txt)
    add_subdirectory(src/i2c)
  endif()
  add_subdirectory(src/shared)

  # host-only scratch pad for experimenting with senders and receivers
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/scratch/CMakeLists.txt)
    add_subdirectory(src/scratch)
  endif()
endif()

# --------------------------------------------------------------------------
# The application is built as a library so that the binary logger's string
# catalog can be generated from it: `gen_str_catalog` runs `nm` over the
# library, and the undefined `catalog<>`/`module<>` specializations it finds
# are exactly the log strings that need IDs. The `demo` executable below is a
# stub containing no log call sites.
add_library(demo_lib STATIC
  src/app.cpp
)
target_include_directories(demo_lib
  PUBLIC
  src/
  src/stm32l432
)
# the detection used by baremetal concurrency results in determining
# that this target is hosted. Just force with define.
target_compile_definitions(demo_lib PUBLIC SIMULATE_FREESTANDING)
target_link_libraries_system(demo_lib PUBLIC async groov stm32-groov cib)

# Generate the string catalog from demo_lib:
#   log_strings.cpp  - the catalog<>/module<> specializations, built into the
#                      demo_log_strings library and linked into the executable
#   log_strings.json - collateral for python/cib/log_decode.py
#   log_strings.xml  - MIPI Sys-T collateral
#
# All three come from one command, so building `demo` produces them all;
# `demo_log_collateral` is just a convenient name for them.
#
# To keep string IDs stable across builds, copy a build's log_strings.json into
# the source tree and pass it back as STABLE_JSON.
gen_str_catalog(
  OUTPUT_CPP
  ${CMAKE_CURRENT_BINARY_DIR}/log_strings.cpp
  OUTPUT_JSON
  ${CMAKE_CURRENT_BINARY_DIR}/log_strings.json
  OUTPUT_XML
  ${CMAKE_CURRENT_BINARY_DIR}/log_strings.xml
  INPUT_LIBS
  demo_lib
  CLIENT_NAME
  "stm32-embedded-demo"
  VERSION
  "${PROJECT_VERSION}"
  OUTPUT_LIB
  demo_log_strings
  OUTPUTS_TARGET
  demo_log_collateral
)

add_executable(demo
  src/main.cpp
)
target_link_libraries(demo PRIVATE demo_lib demo_log_strings)
# --------------------------------------------------------------------------
# --------------------------------------------------------------------------
