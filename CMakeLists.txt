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
add_versioned_package("gh:intel/cpp-std-extensions#a35c5f2")
add_versioned_package("gh:intel/cpp-baremetal-senders-and-receivers#3e3c8aa")
add_versioned_package("gh:intel/generic-register-operation-optimizer#2e45cc2")
add_versioned_package("gh:caisselabs/stm32-groov#6769da8")
add_versioned_package("gh:intel/compile-time-init-build#f9b4cc5")



if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(src/i2c)
  add_subdirectory(src/shared)
endif()

# --------------------------------------------------------------------------
add_executable(demo
  src/main.cpp
)
target_include_directories(demo
  PRIVATE
  src/
  src/stm32l432
)
# the detection used by baremetal concurrency results in determining
# that this target is hosted. Just force with define.
target_compile_definitions(demo PUBLIC SIMULATE_FREESTANDING)
target_link_libraries_system(demo PUBLIC async groov stm32-groov cib)
# --------------------------------------------------------------------------
# --------------------------------------------------------------------------
