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

add_versioned_package("gh:intel/cpp-baremetal-concurrency#63230eb")
add_versioned_package("gh:intel/cpp-std-extensions#c162ade")
add_versioned_package("gh:intel/cpp-baremetal-senders-and-receivers#bd96d20")
add_versioned_package("gh:intel/generic-register-operation-optimizer#d365927")
add_versioned_package("gh:caisselabs/stm32-groov#6769da8")
add_versioned_package("gh:intel/compile-time-init-build#542f94a")



# --------------------------------------------------------------------------
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
