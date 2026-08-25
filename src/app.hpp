// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

// The application proper. `main` is a stub that calls this so that all code
// containing log call sites lives in a library the string catalog can be
// generated from -- see the top-level CMakeLists.txt.
[[noreturn]] auto app_run() -> void;
