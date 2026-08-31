#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Tests for the host-side debugging scripts in this directory.
#
# add_unit_test(... PYTEST ...) dispatches to the infrastructure's
# add_python_test_target, which registers a ctest test named
# PYTHON.unwrap_log_test and hooks it into the `python_tests` target. Since
# `unit_tests` depends on `python_tests`, these run alongside the C++ tests with
# no separate CI job.
#
# The infrastructure does not look for pytest itself, and the test it creates is
# part of ALL -- so without this guard a plain `ninja` in a host build would
# fail for anyone who has no pytest installed. Announce and skip instead,
# matching what the infrastructure does for ruff and mypy.
find_program(PYTEST_PROGRAM "pytest")

if(NOT PYTEST_PROGRAM)
  message(STATUS "pytest not found. Skipping tool tests (tools/test).")
  return()
endif()

message(STATUS "pytest found at ${PYTEST_PROGRAM}")

add_unit_test(unwrap_log_test PYTEST FILES test/test_unwrap_log.py)
