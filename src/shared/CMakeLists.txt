find_package(Threads REQUIRED)

add_unit_test(
  region_test
  CATCH2
  FILES
  test/region_test.cpp
  INCLUDE_DIRECTORIES
  ..
  LIBRARIES
  Threads::Threads
  SYSTEM_LIBRARIES
  async)

# Mutation-test the region abstraction. region_test covers the concurrency
# behaviour that `region` exists to provide, which is exactly the kind of code
# where a passing test suite can still be missing cases -- mull injects
# mutations and reports any that no test catches.
#
# This is a no-op unless the compiler is clang: cmake/mull.cmake in the
# infrastructure defines add_mull_test() as an empty stub and returns early for
# every other compiler. When clang is used but mull itself is not installed,
# `mull_tests` becomes a target that fails with an explanatory message rather
# than silently passing.
add_mull_test(region_test)
